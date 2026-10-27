#include "Scene/SceneInstanceLoader.h"
#include "Diagnostics/Profiler.h"   // SIL.* profiler stages
#include "Resource/PrefabTemplate.h" // a runtime spawn's model records, skeleton reuse, shared parts
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/PhysicsComponents.h"   // .iscn "C " collision records -> Collider/RigidBody
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"
#include "Resource/SkeletonAsset.h"
#include "Resource/SkeletonImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/AssetRefs.h"        // PercentDecode: .iscn name= values
#include "Resource/ModelCollision.h"   // the model's asset collision (BO / PR records)
#include "Resource/MorphLibrary.h"     // every .imorph flavour
#include "Resource/ModelVirtualBones.h" // the model's virtual-bone (VB) records
#include "Resource/SceneSerializer.h"  // ColliderIsSpawnDefault (RefreshAssetCollision)
#include "System/PathUtil.h"
#include "System/CommandLine.h"  // -sockets_asset_off (derive no asset sockets)
#include "Math/MathUtils.h"      // RollPitchYawDegreesFromQuaternion (a derived socket's Euler seed)
#include "Renderer/Renderer.h"
#include "Scene/SceneLoader.h"   // SceneLoader::SkinnedMeshPending
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cctype>
#include <cassert>
#include <chrono>   // [LOAD-PROFILE] per-phase timing of Load()
#include <cstddef>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>   // std::memcpy / std::memchr: .iscn payload handling
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

using namespace DirectX;
namespace fs = std::filesystem;

// ==== Internal types ====
namespace
{
    // Interleaved pos / normal / uv vertex (32 bytes): a view of the first three
    // attribute streams of both the 32-byte layout and the 48-byte with-tangent
    // layout, whose first 32 bytes are identical. Step by the metadata's
    // vertexStride, not sizeof(PackedVertex), to skip the optional trailing tangent.
    struct PackedVertex
    {
        float px, py, pz;   // position  (12 B)
        float nx, ny, nz;   // normal    (12 B)
        float u,  v;        // UV0       ( 8 B)
    };
    static_assert(sizeof(PackedVertex) == 32, "PackedVertex size mismatch");

    struct NodeRecord
    {
        int              idx    { -1 };
        int              parent { -1 };
        float tx { 0 }, ty { 0 }, tz { 0 };
        float qx { 0 }, qy { 0 }, qz { 0 }, qw { 1 };
        float sx { 1 }, sy { 1 }, sz { 1 };
        std::vector<int> meshFileIndices;  // indices into fileRecords[]
        std::string      name;
        uint64_t         meshKey { 0 };    // `mkey=`: the Mesh this node places (0 = none)
    };

    struct FileRecord
    {
        int         idx  { -1 };
        std::string file;          // bare filename (sibling of .iscn)
        std::string matFile;       // optional .imat filename (from mat= field)
        std::string name;          // optional display name (from name=), percent-DECODED
        uint32_t    subIdx { 0 };  // MeshLibrary entry index within `file` (`mesh=` / `sub=`)
        uint64_t    sectionKey { 0 };  // `sec=`: the entry's SectionKey; subIdx is then only a hint
    };

    // "C ...": a collision primitive attached to an F record, i.e. to a mesh ASSET
    // (as DCC tools author it: Unreal's UBX_/UCX_ live on the UStaticMesh, not on
    // the placement). Every node referencing that F gets the same collider, and
    // PhysicsSystem shares one cached Jolt shape across them.
    //
    //   C f=<F idx> shape=<0 Box|1 Sphere|2 Capsule|3 Mesh|4 ConvexHull>
    //     [he=x_y_z] [r=<f>] [hh=<f>] [off=x_y_z] [rot=x_y_z]
    //     [lib=<name.meshlib>] [mesh=<entry>] [trig=<0|1>] [layer=<u>]
    //
    // `lib` is a sibling file of the .iscn holding the collision geometry (Mesh /
    // ConvexHull); omitted means the F record's own meshlib, i.e. collide against
    // the render mesh (Unreal's complex-as-simple). Several C lines may share one `f`
    // to form a compound: the first lands on the mesh entity, the rest become
    // collider-only sibling entities.
    // "V ..." is the same payload attached to a NODE (`n=<node idx>` instead of
    // `f=`): placement-level collision without geometry (Unreal BlockingVolume, a
    // per-actor trigger).
    struct CollisionRecord
    {
        int               fileIdx { -1 };
        int               nodeIdx { -1 };
        uint32_t          shape   { 0 };            // ColliderComponent::Shape raw
        DirectX::XMFLOAT3 halfExtents { 0.5f, 0.5f, 0.5f };
        float             radius     { 0.5f };
        float             halfHeight { 0.5f };
        DirectX::XMFLOAT3 offset   { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 rotEuler { 0.f, 0.f, 0.f };
        std::string       lib;                      // empty = the F record's meshlib
        uint32_t          meshId  { 0 };
        bool              isTrigger { false };
        uint32_t          layer   { 0 };            // ColliderComponent::CollisionLayer raw
    };

    // What a node's parts need to take the model's ASSET collision
    // (Resource/ModelCollision.h): the parsed records and, per F-line, the key of the
    // section it draws and of the Mesh owning it (the owner the `mkey=` stamp names).
    struct AssetCollisionSpawn
    {
        const Resource::ModelCollision* collision = nullptr;
        std::string                     model;         // the MeshAsset collider's `model` (project-relative)
        std::vector<uint64_t>           fiSectionKey;  // per F-line (0: unknown)
        std::vector<uint64_t>           fiMeshKey;     // per F-line (0: unknown)
        uint32_t                        meshBodies = 0, sectionBodies = 0, conflicts = 0;
    };

    // One part of a node as the asset collision sees it: the entity, the Mesh it
    // is a section of, that section, and its library entry (a section trimesh's
    // geometry - the loader's spelling of the library path).
    struct AssetPart
    {
        Entity      ent        = NullEntity;
        uint64_t    meshKey    = 0;
        uint64_t    sectionKey = 0;
        std::string lib;
        uint32_t    entry      = 0;
    };

    // Per-mesh blend data entry stored after deserialization.
    struct IskelMeshBlend
    {
        uint32_t               meshIndex   = 0;
        std::vector<BlendVertex> blendData;
    };

    // Deserialized skeleton data, not registered yet (the caller registers).
    // skeleton is heap-allocated: SkeletonAsset holds fixed MAX_BONES arrays
    // (hundreds of KB) and must not live on the stack.
    struct IskelData
    {
        std::unique_ptr<SkeletonAsset> skeleton;
        std::vector<ClipAsset>         clips;
        std::vector<IskelMeshBlend>    meshBlends;
        bool valid = false;
    };

    // Decoded mesh streams (from a .imsh blob or a .meshlib entry) for skinned
    // mesh registration.
    struct ImshStreams
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        std::vector<DirectX::XMFLOAT2> uvs1;   // 2nd UV set (empty when the source has none)
        std::vector<DirectX::XMFLOAT2> uvs2;   // optional additional UV set (float2)
        std::vector<DirectX::XMFLOAT2> uvs3;   // optional additional UV set (float2)
        // float4 tangent (xyz dir, w handedness). Empty when the source .meshlib
        // lacks MESHLIB_FLAG_HAS_TANGENT.
        std::vector<DirectX::XMFLOAT4> tangents;
        std::vector<uint32_t>           indices;
        bool valid = false;
    };

    // Batch memoisation: the backing store for SceneInstanceLoader::BatchScope.
    // Without it, instantiating the same character N times re-reads and
    // re-decomposes byte-identical payloads N times (AIBench: 100 Rossi.iscn copies,
    // 49-60 ms of skinned+decompose each). The cache lives only while a BatchScope
    // is open (see the header), so it never serves a stale decode after a re-import.
    // Main thread only, like every SceneInstanceLoader::Load caller.
    struct MorphBlock
    {
        uint32_t                       morphCount  = 0;   // LOCAL target count
        // morphCount * vertexCount positions, then (hasNormals) as many normal
        // deltas - the GPU buffer's image (SkinnedMeshPending::morphDeltas).
        std::vector<DirectX::XMFLOAT3> deltas;
        bool                           hasNormals = false;
        std::vector<std::string>       names;
        // local -> skeleton morph index. Synthesised as the identity for a
        // dense block, so the cached answer always has one shape.
        std::vector<uint32_t>          slotOfLocal;
        bool                           found = false;
    };

    // One .iskel's registry-resident result, shared by every instance of the same
    // character within the batch. SkeletonAsset is fixed-size (MAX_BONES arrays,
    // hundreds of KB whatever the bone count) and SkeletonRegistry never frees
    // entries (no Clear(), untouched by OnWorldClear), so a copy per instance would
    // cost tens of MB at 100 instances.
    // Sharing is safe: registered skeletons are written only by
    // HumanoidProfileBuilder::Build, which is content-derived and idempotent (its
    // runtime callers guard it with `!humanoid.IsValid()`), and by the editor's
    // humanoid-mapping edits, which belong to the asset; the retarget path mutates no
    // skeleton asset. The first instance that needs a profile builds it, the rest skip.
    struct IskelShare
    {
        uint32_t                    skelIdx = kInvalidSkeletonIndex;
        std::vector<IskelMeshBlend> meshBlends;   // retained: identical per instance
        std::size_t                 clipCount = 0;
        bool                        valid = false;
    };

    struct BatchCache
    {
        std::unordered_map<std::string, std::vector<uint8_t>> meshlibBlobs;
        std::unordered_map<std::string, std::vector<uint8_t>> imorphBlobs;
        std::unordered_map<std::string, ImshStreams>          streams;  // key: path#meshId
        std::unordered_map<std::string, MorphBlock>           morphs;   // key: path#meshFileIdx
        std::unordered_map<std::string, IskelShare>           iskels;   // key: iskelPath
        std::unordered_map<std::string, std::string>          stamps;   // path -> PrefabTemplate::FileStamp (mixed into sourceKey)
        std::size_t decodedBytes = 0;
        uint32_t    streamHit = 0, streamMiss = 0, morphHit = 0, morphMiss = 0;
        uint32_t    iskelHit = 0, iskelMiss = 0;
    };

    int         g_batchDepth = 0;
    BatchCache* g_batch      = nullptr;

    struct ReusedRegistration
    {
        uint32_t    skelIdx   = kInvalidSkeletonIndex;
        std::size_t clipCount = 0;
    };
    int g_reuseDepth = 0;

    struct TemplateRegistration
    {
        uint32_t              skelIdx   = kInvalidSkeletonIndex;
        std::size_t           clipCount = 0;
        std::vector<uint32_t> skinnedMeshes;   // IskelMeshBlend::meshIndex, in .iskel order
    };

    // Decoded-payload budget. Past it the cache stops accepting new entries
    // (resident ones keep serving), so a scene of hundreds of distinct characters
    // cannot balloon; the redundancy it targets is repeated instances of the same
    // character, resident long before the budget is reached.
    constexpr std::size_t kBatchCacheBudgetBytes = 768ull * 1024 * 1024;

    // The prepare half of Load: the model file read and parsed into its records,
    // pure data (no World, no registry, no GPU). Load instantiates from it; a runtime
    // spawn keeps it across spawns (Resource/PrefabTemplate.h), validated by the
    // file's stamp.
    struct MorphFileRef { int meshFileIdx = -1; std::string file; };   // M idx=<F index> file=<.imorph>

    struct ModelPrep
    {
        std::vector<NodeRecord> nodes;          // sorted by idx: parents are created first
        std::vector<FileRecord> fileRecords;
        // C-lines grouped by the F index they attach to; V-lines by NODE index.
        std::unordered_map<int, std::vector<CollisionRecord>> collisionRecords;
        std::unordered_map<int, std::vector<CollisionRecord>> volumeRecords;
        std::string                 skelRelFile;        // the K line's bare filename ("" = none)
        std::vector<MorphFileRef>   morphFileRefs;      // per-mesh morph target files
        Resource::ModelCollision    modelCollision;     // the BO / PR records (asset collision)
        Resource::ModelSockets      modelSockets;       // the SO records (sockets)
        Resource::ModelVirtualBones modelVirtualBones;  // the VB records (virtual bones)
    };

    struct CachedSpawnPrep { std::string stamp; std::shared_ptr<const ModelPrep> prep; };

    bool ParseCLine(const std::string& line, CollisionRecord& out)
    {
        std::istringstream ss(line.substr(2));  // skip "C "
        std::string token;
        const auto v3 = [](const std::string& s, DirectX::XMFLOAT3& dst)
        {
            float x = 0, y = 0, z = 0;
            if (sscanf_s(s.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) dst = { x, y, z };
        };
        while (ss >> token)
        {
            const auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = token.substr(0, eq);
            const std::string val = token.substr(eq + 1);
            if (val.empty()) continue;
            try
            {
                if      (key == "f")     out.fileIdx    = std::stoi(val);
                else if (key == "n")     out.nodeIdx    = std::stoi(val);
                else if (key == "shape") out.shape      = static_cast<uint32_t>(std::stoul(val));
                else if (key == "he")    v3(val, out.halfExtents);
                else if (key == "r")     out.radius     = std::stof(val);
                else if (key == "hh")    out.halfHeight = std::stof(val);
                else if (key == "off")   v3(val, out.offset);
                else if (key == "rot")   v3(val, out.rotEuler);
                else if (key == "lib")   out.lib        = val;
                else if (key == "mesh")  out.meshId     = static_cast<uint32_t>(std::stoul(val));
                else if (key == "trig")  out.isTrigger  = (std::stoi(val) != 0);
                else if (key == "layer") out.layer      = static_cast<uint32_t>(std::stoul(val));
            }
            catch (...) {}
        }
        return out.fileIdx >= 0 || out.nodeIdx >= 0;
    }

    // CollisionRecord -> ColliderComponent. @p fallbackLib / @p fallbackMeshId
    // supply the geometry when the record names no `lib` (collide against the owning
    // F record's render mesh). Returns false when a geometry-backed shape resolves to
    // no source at all.
    bool MakeColliderFromRecord(const CollisionRecord&       cr,
                                const std::string&           fallbackLib,
                                uint32_t                     fallbackMeshId,
                                const std::filesystem::path& iscnDir,
                                ColliderComponent&           out)
    {
        out = ColliderComponent{};
        out.shape            = static_cast<ColliderComponent::Shape>(cr.shape);
        out.halfExtents      = cr.halfExtents;
        out.radius           = cr.radius;
        out.halfHeight       = cr.halfHeight;
        out.offset           = cr.offset;
        out.rotationEulerDeg = cr.rotEuler;
        out.isTrigger        = cr.isTrigger;
        out.layer            = static_cast<ColliderComponent::CollisionLayer>(cr.layer);

        if (out.shape == ColliderComponent::Shape::Mesh ||
            out.shape == ColliderComponent::Shape::ConvexHull)
        {
            if (cr.lib.empty())
            {
                out.meshLibPath   = fallbackLib;
                out.meshLibMeshId = fallbackMeshId;
            }
            else
            {
                // PhysicsSystem re-reads the file itself, so store the canonical
                // project-relative form (same treatment as the .imat path).
                out.meshLibPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                    (iscnDir / Sys::U8Path(cr.lib)).lexically_normal()));
                out.meshLibMeshId = cr.meshId;
            }
            if (out.meshLibPath.empty()) return false;
        }
        return true;
    }

    // The one definition of a node's child order. SpawnSingleNode creates a node's
    // child entities in this fixed sequence, all pushed into the node's Children:
    //
    //     [ _vol1 .. _volN ]  [ mesh, _col1 .. _colM ] x each accepted F ref
    //
    // and the caller's DFS then appends each child node's subtree.
    // PrefabSerializer::LoadPrefab keys every saved per-part component block
    // (MaterialRef included) on an entity's POSITION in that pre-order DFS
    // (entityMap[sceneIdx + i] = loadedEntities[i]), so ReadMeshPlacements must agree
    // exactly: both walk this function. It owns the order and the "first rides on its
    // parent, the rest get their own entity" rules; a sink owns what a slot means
    // (create an entity / consume a DFS index).
    // The sinks are template parameters, not std::function: this runs once per node
    // of every .iscn a load touches (7,054 nodes in a Tokyo-class import), and type
    // erasure would cost three heap allocations per node.
    //   volume(cr, acceptedIndex)  A "V " record on this node. acceptedIndex counts
    //       the records that became colliders so far: 0 rides on the node entity (no
    //       DFS slot), 1.. get their own `_volN` entity. Return false when the record
    //       does not become a collider (MakeColliderFromRecord failed); it then
    //       creates no entity and does not advance the index.
    //   mesh(fi)                   One "F " reference of this node. Return false when
    //       it does not become a mesh entity (index out of range, or in the spawn a
    //       mesh library that failed to load).
    //   collider(cr, fi, primIdx)  A "C " record on that F reference; same first/rest
    //       rule as volume, accepted index 0 rides on the mesh entity.
    // @p volumeRecords and @p collisionRecords may be null.
    template <class VolumeSink, class MeshSink, class ColliderSink>
    void WalkNodeChildSlots(
        const NodeRecord&                                            rec,
        const std::unordered_map<int, std::vector<CollisionRecord>>*  volumeRecords,
        const std::unordered_map<int, std::vector<CollisionRecord>>*  collisionRecords,
        VolumeSink&&                                                 volume,
        MeshSink&&                                                   mesh,
        ColliderSink&&                                               collider)
    {
        // ---- Placement-level collision ("V " records) ----
        // Geometry-free volumes (Unreal BlockingVolume / per-actor triggers), first:
        // SpawnSingleNode puts them before the meshes, in the node's first child slots.
        if (volumeRecords)
        {
            const auto vIt = volumeRecords->find(rec.idx);
            if (vIt != volumeRecords->end())
            {
                int volIdx = 0;
                for (const CollisionRecord& cr : vIt->second)
                    if (volume(cr, volIdx)) ++volIdx;
            }
        }

        // ---- One mesh entity per mesh file reference ----
        for (int fi : rec.meshFileIndices)
        {
            if (!mesh(fi)) continue;
            if (!collisionRecords) continue;
            const auto crIt = collisionRecords->find(fi);
            if (crIt == collisionRecords->end()) continue;
            int primIdx = 0;
            for (const CollisionRecord& cr : crIt->second)
                if (collider(cr, fi, primIdx)) ++primIdx;
        }
    }

    // Parse one "N ..." line into a NodeRecord.
    // Format: N idx=i parent=j tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=..
    //           sx=.. sy=.. sz=.. mc=k mr0=fi ... [mkey=<hex16>] name=name
    bool ParseNLine(const std::string& line, NodeRecord& out)
    {
        std::istringstream ss(line.substr(2));  // skip "N "
        std::string token;
        while (ss >> token)
        {
            const auto eq = token.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = token.substr(0, eq);
            const std::string val = token.substr(eq + 1);
            if (val.empty()) continue;

            try
            {
                if      (key == "idx")    out.idx    = std::stoi(val);
                else if (key == "parent") out.parent = std::stoi(val);
                else if (key == "tx")     out.tx     = std::stof(val);
                else if (key == "ty")     out.ty     = std::stof(val);
                else if (key == "tz")     out.tz     = std::stof(val);
                else if (key == "qx")     out.qx     = std::stof(val);
                else if (key == "qy")     out.qy     = std::stof(val);
                else if (key == "qz")     out.qz     = std::stof(val);
                else if (key == "qw")     out.qw     = std::stof(val);
                else if (key == "sx")     out.sx     = std::stof(val);
                else if (key == "sy")     out.sy     = std::stof(val);
                else if (key == "sz")     out.sz     = std::stof(val);
                else if (key == "mc")     { /* count, mesh refs follow */ }
                else if (key == "name")
                {
                    // `name=` is always the last token every writer emits, so take the rest of
                    // the line: the whitespace tokenizer would cut "name=GirlsFrontline LindSummer"
                    // at the space and re-read the remainder as key=value pairs (a name containing
                    // " tx=9" would overwrite the node's translation).
                    const size_t np = line.find(" name=");
                    if (np != std::string::npos)
                    {
                        std::string rest = line.substr(np + 6);
                        while (!rest.empty() && (rest.back() == ' '  ||
                                                 rest.back() == '\r' ||
                                                 rest.back() == '\n'))
                            rest.pop_back();
                        // Percent-decode unconditionally: a no-op on raw names (the writers
                        // sanitise '%' away), and it lets a writer encode.
                        out.name = Resource::AssetRefs::PercentDecode(rest);
                    }
                    else
                        out.name = val;
                    break;   // nothing follows `name=`: stop tokenizing
                }
                else if (key == "mkey")
                {
                    // Informational (the stamp tool and the tools read it); the spawn
                    // follows the F-lines' own keys.
                    uint64_t k = 0;
                    if (Resource::MeshKeyFromHex(val, k)) out.meshKey = k;
                }
                else if (key.size() > 2 && key[0] == 'm' && key[1] == 'r')
                    out.meshFileIndices.push_back(std::stoi(val));
            }
            catch (...) {}
        }
        return out.idx >= 0;
    }

    // Parse one "K ..." line, a skeleton file reference.
    // Format: K file=filename.iskel
    bool ParseKLine(const std::string& line, std::string& outFile)
    {
        const size_t filePos = line.find("file=");
        if (filePos == std::string::npos) return false;
        outFile = line.substr(filePos + 5);
        // trim trailing whitespace / CR
        while (!outFile.empty() && (outFile.back() == '\r' || outFile.back() == '\n' ||
                                     outFile.back() == ' '))
            outFile.pop_back();
        return !outFile.empty();
    }

    // Parse one "F ..." line into a FileRecord (grammar below).
    bool ParseFLine(const std::string& line, FileRecord& out)
    {
        size_t idxPos = line.find("idx=");
        if(idxPos != std::string::npos) {
            size_t spaceAfterIdx = line.find(' ', idxPos);
            std::string val = line.substr(idxPos + 4, spaceAfterIdx - (idxPos + 4));
            try { out.idx = std::stoi(val); }
            catch (...) {}
        }
        // F line layout:
        //   F idx=I file=<name.meshlib> mesh=M [sec=<hex16>] [name=<pct-encoded>] [mat=<name.imat>]
        // `mesh=M` is the MeshLibraryEntry index (meshId); `sub=` is accepted in its
        // place (same subIdx field).
        // Key order is part of the grammar: `mat=` runs to end of line so material
        // filenames may contain spaces, so it must be LAST (a key after it would be
        // swallowed into the path and every material on the model would fail to load).
        // Every other value ends at the position of the next known keyword, never at a
        // space.
        // `name=` is optional and percent-encoded (space, '=' and every UTF-8 byte
        // escaped); without it the caller's default name is used.
        size_t filePos = line.find("file=");
        size_t subPos  = line.find(" mesh=");
        if (subPos == std::string::npos) subPos = line.find(" sub=");
        size_t matPos  = line.find(" mat=");
        // Look for " name=" only in the region BEFORE " mat=": the material path
        // runs to end of line and could legitimately contain that byte sequence.
        size_t namePos = line.find(" name=");
        if (namePos != std::string::npos && matPos != std::string::npos && namePos > matPos)
            namePos = std::string::npos;
        // `sec=<hex16>`: the entry's SectionKey, written after `mesh=` and before
        // `name=` / `mat=` (as for name=, only the region before " mat=" is searched). A
        // parser that does not know the key reads `mesh=`'s value up to the next key it
        // knows ("3 sec=..."), and stoul still reads 3.
        size_t secPos  = line.find(" sec=");
        if (secPos != std::string::npos && matPos != std::string::npos && secPos > matPos)
            secPos = std::string::npos;

        // Smallest keyword position strictly after @p from, i.e. where the value
        // starting at @p from ends.
        auto nextKeyPos = [&](size_t from) -> size_t
        {
            size_t best = std::string::npos;
            for (size_t kp : { subPos, secPos, namePos, matPos })
                if (kp != std::string::npos && kp > from && kp < best) best = kp;
            return best;
        };

        if (filePos != std::string::npos)
        {
            size_t fileStart = filePos + 5;
            size_t fileEnd   = nextKeyPos(fileStart);

            out.file = (fileEnd == std::string::npos)
                ? line.substr(fileStart)
                : line.substr(fileStart, fileEnd - fileStart);

            while (!out.file.empty() && (out.file.back() == ' ' || out.file.back() == '\r' || out.file.back() == '\n'))
                out.file.pop_back();
        }

        if (subPos != std::string::npos)
        {
            // " mesh=" is 6 chars, " sub=" is 5: detect which one matched.
            const bool isMesh  = (line.compare(subPos, 6, " mesh=") == 0);
            const size_t skip  = isMesh ? 6u : 5u;
            size_t subStart    = subPos + skip;
            size_t subEnd      = nextKeyPos(subStart);
            if (subEnd == std::string::npos) subEnd = line.find(' ', subStart);
            std::string val = (subEnd == std::string::npos)
                ? line.substr(subStart)
                : line.substr(subStart, subEnd - subStart);
            try { out.subIdx = static_cast<uint32_t>(std::stoul(val)); }
            catch (...) { out.subIdx = 0; }
        }

        if (secPos != std::string::npos)
        {
            const size_t s = secPos + 5;   // skip " sec="
            size_t e = nextKeyPos(s);
            if (e == std::string::npos) e = line.find(' ', s);
            std::string val = (e == std::string::npos) ? line.substr(s) : line.substr(s, e - s);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n'))
                val.pop_back();
            uint64_t k = 0;
            if (Resource::MeshKeyFromHex(val, k)) out.sectionKey = k;
        }

        if (namePos != std::string::npos)
        {
            const size_t nameStart = namePos + 6;   // skip " name="
            size_t nameEnd = nextKeyPos(nameStart);
            if (nameEnd == std::string::npos) nameEnd = line.find(' ', nameStart);
            std::string raw = (nameEnd == std::string::npos)
                ? line.substr(nameStart)
                : line.substr(nameStart, nameEnd - nameStart);
            while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\r' || raw.back() == '\n'))
                raw.pop_back();
            out.name = Resource::AssetRefs::PercentDecode(raw);
        }

        if (matPos != std::string::npos)
        {
            size_t matStart = matPos + 5; // skip " mat="
            out.matFile = line.substr(matStart);
            while (!out.matFile.empty() && (out.matFile.back() == ' ' || out.matFile.back() == '\r' || out.matFile.back() == '\n'))
                out.matFile.pop_back();
        }

        return !out.file.empty();

    }

    // The rules, shared by the spawn and a live refresh (RefreshAssetCollision): per
    // Mesh a node places (parts grouped by owning Mesh, in first-part order), the
    // BODY prims become one MeshAsset collider on that Mesh's first part, and each
    // own-library section trimesh goes on the part that draws that section
    // (complexity=complex: all of them). A part that already has a collider (a C
    // record naming the same F-line, an edit) keeps it and counts as a conflict.
    // Every part given one is appended to @p given.
    void GiveAssetCollision(World& world, const Resource::ModelCollision& mc, const std::string& model,
                            const std::vector<AssetPart>& nodeParts, AssetCollisionSpawn& ac,
                            std::vector<Entity>& given, uint32_t* outColliderCount)
    {
        std::vector<uint64_t> order;   // Meshes in first-part order
        for (const AssetPart& p : nodeParts)
            if (p.meshKey != 0 && std::find(order.begin(), order.end(), p.meshKey) == order.end())
                order.push_back(p.meshKey);
        for (uint64_t mk : order)
        {
            const Resource::MeshBodySetup* setup = mc.Find(mk);
            if (!setup || setup->complexity == Resource::MeshBodySetup::Complexity::None) continue;
            std::vector<const AssetPart*> parts;
            std::vector<uint64_t>         partSections;
            for (const AssetPart& p : nodeParts)
                if (p.meshKey == mk)
                {
                    parts.push_back(&p);
                    partSections.push_back(p.sectionKey);
                }
            ColliderComponent base{};
            base.layer     = static_cast<ColliderComponent::CollisionLayer>(setup->layer);
            base.isTrigger = setup->trigger;
            RigidBodyComponent rb{};
            rb.motion = RigidBodyComponent::Motion::Static;
            if (setup->friction >= 0.f) rb.friction = setup->friction;
            if (setup->restitution >= 0.f) rb.restitution = setup->restitution;
            auto give = [&](Entity ent, const ColliderComponent& col) -> bool
            {
                if (world.HasComponent<ColliderComponent>(ent)) { ++ac.conflicts; return false; }
                world.AddComponent<ColliderComponent>(ent, col);
                if (!world.HasComponent<RigidBodyComponent>(ent)) world.AddComponent<RigidBodyComponent>(ent, rb);
                if (outColliderCount) ++(*outColliderCount);
                given.push_back(ent);
                return true;
            };
            if (setup->HasBody())
            {
                ColliderComponent col = base;
                col.shape        = ColliderComponent::Shape::MeshAsset;
                col.assetModel   = model;
                col.assetMeshKey = mk;
                if (give(parts.front()->ent, col)) ++ac.meshBodies;
            }
            const std::vector<uint64_t> secs = setup->SectionTrimeshes(partSections);
            for (const AssetPart* p : parts)
            {
                if (std::find(secs.begin(), secs.end(), p->sectionKey) == secs.end()) continue;
                ColliderComponent col = base;
                col.shape         = ColliderComponent::Shape::Mesh;
                col.meshLibPath   = p->lib;
                col.meshLibMeshId = p->entry;
                if (!col.meshLibPath.empty() && give(p->ent, col)) ++ac.sectionBodies;
            }
        }
    }

    // Create the ECS entities for ONE node record: the node entity, its volume /
    // mesh / compound-collider children and the model's asset collision. Does not
    // recurse: the caller (SceneInstanceLoader::Load) drives an iterative DFS, since
    // this function's large frame overflows the 1 MB stack on deep skeletons (see
    // there). Sets entityMap[rec.idx] to the node entity.
    Entity SpawnSingleNode(int                                        nodeIdx,
                     const std::vector<NodeRecord>&                   nodes,
                     // Per-F-line resolved data from MeshLibrary::GetEntry
                     const std::vector<Resource::Handle>&             fiLibHandle,
                     const std::vector<uint32_t>&                     fiMeshId,
                     const std::vector<uint32_t>&                     fiIndexCount,
                     const std::vector<uint32_t>&                     fiVertexCount,
                     const std::vector<DirectX::XMFLOAT3>&            fiAabbMin,
                     const std::vector<DirectX::XMFLOAT3>&            fiAabbMax,
                     const std::vector<std::string>&                  meshAbsPaths,
                     const std::vector<FileRecord>&                   fileRecords,
                     const std::filesystem::path&                     iscnDir,
                     World&                                           world,
                     Resource::AssetManager&                          assetMgr,
                     std::unordered_map<int, Entity>&                 entityMap,
                     std::unordered_map<std::string, MaterialComponent>& matCache,
                     uint32_t&                                        outNodeCount,
                     uint32_t&                                        outMeshCount,
                     std::unordered_map<int, std::vector<Entity>>*    meshFileToEntities = nullptr,
                     const std::unordered_map<int, std::vector<CollisionRecord>>* collisionRecords = nullptr,
                     uint32_t*                                        outColliderCount = nullptr,
                     const std::unordered_map<int, std::vector<CollisionRecord>>* volumeRecords = nullptr,
                     AssetCollisionSpawn*                             assetCollision = nullptr)
    {
        const NodeRecord& rec = nodes[nodeIdx];
        std::vector<std::pair<int, Entity>> nodeParts;   // (F index, part entity), in ref order
        std::vector<Entity>                 assetGiven;  // parts the asset collision went on

        // ---- Node entity ----
        const Entity nodeEnt = world.CreateEntity();
        world.SetName(nodeEnt, rec.name.empty() ? "node" : rec.name);
        entityMap[rec.idx] = nodeEnt;
        ++outNodeCount;

        LocalTransform lt;
        lt.translation = { rec.tx, rec.ty, rec.tz };
        lt.rotation    = { rec.qx, rec.qy, rec.qz, rec.qw };
        lt.scale       = { rec.sx, rec.sy, rec.sz };

        world.AddComponent<LocalTransform> (nodeEnt, lt);
        world.AddComponent<GlobalTransform>(nodeEnt, GlobalTransform{});
        world.AddComponent<VisibilityComponent>(nodeEnt, VisibilityComponent{});
        world.AddComponent<RenderLayer>    (nodeEnt, RenderLayer{});
        world.AddComponent<SceneNodeTag>   (nodeEnt, SceneNodeTag{});
        world.AddComponent<Children>       (nodeEnt, Children{});

        if (rec.parent >= 0)
        {
            auto it = entityMap.find(rec.parent);
            if (it != entityMap.end())
            {
                world.AddComponent<Parent>(nodeEnt, Parent{ it->second });
                if (Children* parentCh = world.GetComponent<Children>(it->second))
                    parentCh->entities.push_back(nodeEnt);
            }
        }

        // ---- The node's child entities, in the one canonical order ----
        // Volumes first, then one entity per accepted mesh ref, each followed by its
        // extra compound-collision primitives. That sequence is the prefab's per-part
        // override key, so it is walked (WalkNodeChildSlots, as ReadMeshPlacements
        // does), never re-written here.
        // Placement-level collision ("V " records): the first geometry-free volume lands
        // on the node entity, which carries the placement transform; the rest get
        // `_volN` children with an identity transform.
        auto slotVolume = [&](const CollisionRecord& cr, int volIdx) -> bool
        {
            ColliderComponent col{};
            if (!MakeColliderFromRecord(cr, std::string(), 0, iscnDir, col))
                return false;
            RigidBodyComponent rb{};
            rb.motion = RigidBodyComponent::Motion::Static;

            Entity volEnt = nodeEnt;
            if (volIdx > 0)
            {
                volEnt = world.CreateEntity();
                world.SetName(volEnt, rec.name + "_vol" + std::to_string(volIdx));
                world.AddComponent<LocalTransform> (volEnt, LocalTransform{});
                world.AddComponent<GlobalTransform>(volEnt, GlobalTransform{});
                world.AddComponent<Parent>         (volEnt, Parent{ nodeEnt });
                if (Children* nch = world.GetComponent<Children>(nodeEnt))
                    nch->entities.push_back(volEnt);
            }
            world.AddComponent<ColliderComponent> (volEnt, col);
            world.AddComponent<RigidBodyComponent>(volEnt, rb);
            if (outColliderCount) ++(*outColliderCount);
            return true;
        };

        // ---- One mesh entity per mesh file reference ----
        Entity lastMeshEnt = NullEntity;
        auto slotMesh = [&](int fi) -> bool
        {
            if (fi < 0 || fi >= static_cast<int>(fiLibHandle.size())) return false;
            const Resource::Handle libH = fiLibHandle[fi];
            if (!libH.IsValid()) return false;

            const Entity meshEnt = world.CreateEntity();
            lastMeshEnt = meshEnt;
            // Prefer the importer's per-F-record name: PMX binds one material per
            // face range, so that is the only per-part label the format has.
            // Without it every part lands in the Outliner as an identical
            // "<node name>_mesh", and the systems that match entities by name (the
            // editor's DuplicateSceneInstance merge, SaveGame's unique-name index)
            // cannot tell them apart. A .iscn without name= keeps "<node name>_mesh".
            {
                const bool haveRec = (fi < static_cast<int>(fileRecords.size()));
                if (haveRec && !fileRecords[fi].name.empty())
                    world.SetName(meshEnt, fileRecords[fi].name);
                else
                    world.SetName(meshEnt, rec.name + "_mesh");
            }
            ++outMeshCount;

            // MeshLibRef carries the library handle + meshId; the renderer
            // dereferences the library entry at draw time for the actual range.
            MeshLibRef ref;
            ref.libHandle = libH;
            ref.meshId    = fiMeshId[fi];

            world.AddComponent<LocalTransform> (meshEnt, LocalTransform{});
            world.AddComponent<GlobalTransform>(meshEnt, GlobalTransform{});
            world.AddComponent<MeshLibRef>     (meshEnt, ref);
            // Per-mesh AABB from the library entry. An entry without a computed
            // AABB gives zero: harmless for rendering, a degenerate BVH leaf.
            WorldAabb meshAabb;
            meshAabb.min = fiAabbMin[fi];
            meshAabb.max = fiAabbMax[fi];
            world.AddComponent<LocalAabb>      (meshEnt, LocalAabb{ meshAabb.min, meshAabb.max });
            world.AddComponent<WorldAabb>      (meshEnt, meshAabb);
            world.AddComponent<VisibilityComponent>(meshEnt, VisibilityComponent{});
            world.AddComponent<RenderLayer>    (meshEnt, RenderLayer{});
            world.AddComponent<Parent>         (meshEnt, Parent{ nodeEnt });
            // Material from the F line's .imat, else the default. matCache
            // matters: Bistro has 22k F-lines but ~200 unique materials, and
            // re-parsing each .imat dozens of times would dominate load time. The
            // key is the canonical asset path, so AssetFS/package lookup is
            // deterministic and prefab MaterialRef replay sees the material as
            // loaded instead of parsing a slash-normalised alias. Each entity gets
            // its own MaterialComponent copy so per-entity edits do not bleed.
            if (fi >= 0 && fi < static_cast<int>(fileRecords.size()) && !fileRecords[fi].matFile.empty())
            {
                const std::string matPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                    (iscnDir / Sys::U8Path(fileRecords[fi].matFile)).lexically_normal()));
                auto it = matCache.find(matPath);
                if (it == matCache.end())
                {
                    MaterialComponent loaded{};
                    const bool ok = Resource::LoadMaterial(matPath, loaded);
                    auto inserted = matCache.emplace(matPath,
                                                     ok ? std::move(loaded) : MaterialComponent{});
                    it = inserted.first;
                }
                world.AddComponent<MaterialComponent>(meshEnt, it->second);
                world.AddComponent<MaterialSourcePath>(meshEnt, MaterialSourcePath{ matPath });
                // Remember what the model said so SaveScene can tell an artist's
                // swap from the model's own default and write a diff. Runtime only,
                // never serialized (see MaterialSpawnDefault).
                world.AddComponent<MaterialSpawnDefault>(meshEnt, MaterialSpawnDefault{ matPath, {} });
            }
            else
                world.AddComponent<MaterialComponent>(meshEnt, MaterialComponent{});

            if (fi >= 0 && fi < static_cast<int>(meshAbsPaths.size()) && !meshAbsPaths[fi].empty())
                world.AddComponent<MeshSourcePath>(meshEnt, MeshSourcePath{ meshAbsPaths[fi] });

            if (meshFileToEntities)
                (*meshFileToEntities)[fi].push_back(meshEnt);
            nodeParts.emplace_back(fi, meshEnt);

            if (Children* nodeCh = world.GetComponent<Children>(nodeEnt))
                nodeCh->entities.push_back(meshEnt);
            return true;
        };

        // ---- Collision primitives declared for this F record ----
        // The first primitive rides on the mesh entity (as the editor's "Use baked as
        // collision" does, so the .iscene MC override keys onto it). Extra compound
        // primitives become collider-only children of the node, which is why they take
        // DFS slots between one mesh entity and the next.
        auto slotCollider = [&](const CollisionRecord& cr, int fi, int primIdx) -> bool
        {
            ColliderComponent col{};
            if (!MakeColliderFromRecord(
                    cr,
                    (fi >= 0 && fi < static_cast<int>(meshAbsPaths.size()))
                        ? meshAbsPaths[fi] : std::string(),
                    (fi >= 0 && fi < static_cast<int>(fiMeshId.size())) ? fiMeshId[fi] : 0u,
                    iscnDir, col))
                return false;   // geometry-backed shape with no resolvable source

            // Imported level geometry is static; Jolt's MeshShape requires it
            // anyway, and a hull on a static body is the cheap common case.
            RigidBodyComponent rb{};
            rb.motion = RigidBodyComponent::Motion::Static;

            Entity colEnt = lastMeshEnt;
            if (primIdx > 0)
            {
                // Compound member: own entity, identity local transform, so
                // it inherits the node's world placement exactly.
                colEnt = world.CreateEntity();
                world.SetName(colEnt, rec.name + "_col" + std::to_string(primIdx));
                world.AddComponent<LocalTransform> (colEnt, LocalTransform{});
                world.AddComponent<GlobalTransform>(colEnt, GlobalTransform{});
                world.AddComponent<Parent>         (colEnt, Parent{ nodeEnt });
                if (Children* nodeCh2 = world.GetComponent<Children>(nodeEnt))
                    nodeCh2->entities.push_back(colEnt);
            }
            if (colEnt == NullEntity) return false;
            world.AddComponent<ColliderComponent> (colEnt, col);
            world.AddComponent<RigidBodyComponent>(colEnt, rb);
            if (outColliderCount) ++(*outColliderCount);
            return true;
        };

        WalkNodeChildSlots(rec, volumeRecords, collisionRecords,
                           slotVolume, slotMesh, slotCollider);

        // ---- The model's asset collision on this node's parts ----
        // See GiveAssetCollision: per Mesh this node places, the body prims on its first
        // part here (where a C record's first primitive rode) and each own-library
        // section trimesh on the part drawing that section. A part that already has a
        // collider (a C record naming the same F-line; the migration never writes both)
        // keeps it.
        if (assetCollision && assetCollision->collision && !nodeParts.empty())
        {
            AssetCollisionSpawn& ac = *assetCollision;
            std::vector<AssetPart> parts;
            parts.reserve(nodeParts.size());
            for (const auto& [fi, ent] : nodeParts)
            {
                AssetPart p;
                p.ent = ent;
                if (fi >= 0 && fi < static_cast<int>(ac.fiMeshKey.size()))
                {
                    p.meshKey    = ac.fiMeshKey[fi];
                    p.sectionKey = ac.fiSectionKey[fi];
                }
                if (fi >= 0 && fi < static_cast<int>(meshAbsPaths.size())) p.lib = meshAbsPaths[fi];
                if (fi >= 0 && fi < static_cast<int>(fiMeshId.size())) p.entry = fiMeshId[fi];
                parts.push_back(std::move(p));
            }
            GiveAssetCollision(world, *ac.collision, ac.model, parts, ac, assetGiven, outColliderCount);
        }

        // ---- What each part was given (SaveScene's MC baseline) ----
        for (const auto& [fi, ent] : nodeParts)
        {
            const ColliderComponent*  col = world.GetComponent<ColliderComponent>(ent);
            const RigidBodyComponent* rb  = world.GetComponent<RigidBodyComponent>(ent);
            if (col && rb)
                world.AddComponent<ColliderSpawnDefault>(
                    ent, ColliderSpawnDefault{ *col, *rb,
                                               std::find(assetGiven.begin(), assetGiven.end(), ent) != assetGiven.end() });
        }

        // Children are spawned by the caller's iterative DFS, not here.
        return nodeEnt;
    }

    // Load and deserialize a .iskel binary blob produced by SceneImporter.
    IskelData LoadIskel(const std::string& iskelPath)
    {
        PROF_SCOPE("SIL.LoadIskel");
        IskelData out;

        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(iskelPath, blob))
        {
            LOG_ERROR("SceneInstanceLoader: cannot open .iskel '%s'", iskelPath.c_str());
            return out;
        }
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SKELETON))
        {
            LOG_ERROR("SceneInstanceLoader: bad .iskel header in '%s'", iskelPath.c_str());
            return out;
        }

        const Resource::SkeletonMetadata* meta =
            Resource::GetMetadata<Resource::SkeletonMetadata>(blob.data());
        const uint32_t boneCount       = meta->boneCount;
        const uint32_t clipCount       = meta->clipCount;
        const uint32_t meshBlendCount  = meta->reserved[0];

        if (boneCount > SkeletonAsset::MAX_BONES)
        {
            LOG_ERROR("SceneInstanceLoader: .iskel '%s' has %u bones but MAX_BONES=%u — re-export required",
                      iskelPath.c_str(), boneCount, SkeletonAsset::MAX_BONES);
            return out;
        }

        const uint8_t* src = Resource::GetPayload(blob.data());
        const uint8_t* dataEnd = blob.data() + blob.size();

        // meshBlendCount in payload
        uint32_t payloadMBC = 0;
        std::memcpy(&payloadMBC, src, sizeof(uint32_t)); src += sizeof(uint32_t);

        // Heap-allocate SkeletonAsset: its fixed MAX_BONES arrays would overflow the stack.
        out.skeleton = std::make_unique<SkeletonAsset>();
        SkeletonAsset& sk = *out.skeleton;
        sk.boneCount = boneCount;

        // parentIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.parentIndex[i], src, sizeof(int32_t)); src += sizeof(int32_t); }

        // inverseBindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.inverseBindPose[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // bindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.bindPose[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // restPoseLocal
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(&sk.restPoseLocal[i], src, 16 * sizeof(float)); src += 16 * sizeof(float); }

        // boneNames + rebuild nameToIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            std::memcpy(sk.boneNames[i], src, 64); src += 64;
            uint32_t hash = 2166136261u;
            for (const char* p = sk.boneNames[i]; *p; ++p)
                hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
            sk.nameToIndex[hash] = i;
        }

        // boneRestAABBs: 6 floats per bone, an optional section.
        // Presence is decided by a discriminating walk, not by "the section fits before
        // EOF": in a file without it the clip / blend data behind would supply the
        // bytes, 24*boneCount bytes of clip data would be eaten as AABBs and every stream
        // after would parse garbage. walkFits simulates the clip + blend headers under
        // each hypothesis; count fields are small, misaligned bytes read as huge floats,
        // so exactly one hypothesis survives for any file with clips or blends. If both
        // survive (neither clips nor blends) the with-section reading wins, as every
        // writer emits the section.
        const auto walkFits = [&](const uint8_t* p) -> bool
        {
            for (uint32_t ci = 0; ci < clipCount; ++ci)
            {
                if (p + 16 > dataEnd) return false;
                uint32_t bc = 0, fc = 0;
                std::memcpy(&bc, p, 4);
                std::memcpy(&fc, p + 4, 4);
                if (bc > SkeletonAsset::MAX_BONES || fc > 1000000u) return false;
                p += 16 + (size_t)bc * fc * 10 * sizeof(float);
                if (p > dataEnd) return false;
            }
            for (uint32_t m = 0; m < payloadMBC; ++m)
            {
                if (p + 8 > dataEnd) return false;
                uint32_t vc = 0;
                std::memcpy(&vc, p + 4, 4);
                if (vc > 5000000u) return false;
                p += 8 + (size_t)vc * sizeof(BlendVertex);
                if (p > dataEnd) return false;
            }
            return true;
        };
        const size_t aabbSectionBytes = (size_t)boneCount * 6 * sizeof(float);
        const bool aabbSectionPresent =
            src + aabbSectionBytes <= dataEnd &&
            (walkFits(src + aabbSectionBytes) || !walkFits(src));

        // Trust the section's values only when the writer stamped
        // ISKEL_FLAG_BONE_AABB_MODEL: unstamped files carry unfilled +-1e30 seeds or
        // bone-LOCAL boxes, while the renderer's bone-AABB merge transforms these boxes
        // by skin matrices, which needs bind-MODEL space. Trusting such a section
        // collapses a rest-pose character's merged hull to a point at the model origin
        // and frustum-culls the whole character.
        if (aabbSectionPresent)
        {
            uint32_t validAabbs = 0;
            for (uint32_t i = 0; i < boneCount; ++i)
            {
                std::memcpy(&sk.boneRestAABBs[i].localMin, src, 3 * sizeof(float)); src += 3 * sizeof(float);
                std::memcpy(&sk.boneRestAABBs[i].localMax, src, 3 * sizeof(float)); src += 3 * sizeof(float);
                const auto& a = sk.boneRestAABBs[i];
                if (a.localMin.x <= a.localMax.x &&
                    a.localMin.y <= a.localMax.y &&
                    a.localMin.z <= a.localMax.z)
                    ++validAabbs;
            }
            sk.hasBoneAABBs =
                (meta->reserved[1] & Resource::ISKEL_FLAG_BONE_AABB_MODEL) != 0 &&
                validAabbs > 0;
            if (!sk.hasBoneAABBs)
                LOG_INFO("SceneInstanceLoader: '%s' bone AABBs unusable (flags=0x%x, valid=%u/%u) "
                         "— skinned bounds fall back to the joint hull; re-import to tighten",
                         iskelPath.c_str(), meta->reserved[1], validAabbs, boneCount);
        }
        else
        {
            LOG_INFO("SceneInstanceLoader: '%s' has no bone-AABB section (pre-section file) "
                     "— skinned bounds fall back to the joint hull; re-import to tighten",
                     iskelPath.c_str());
        }

        // clips
        out.clips.resize(clipCount);
        for (uint32_t ci = 0; ci < clipCount; ++ci)
        {
            ClipAsset& clip = out.clips[ci];
            uint32_t bc = 0, fc = 0;
            std::memcpy(&bc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&fc,          src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&clip.duration,  src, sizeof(float)); src += sizeof(float);
            std::memcpy(&clip.frameRate, src, sizeof(float)); src += sizeof(float);
            clip.boneCount  = bc;
            clip.frameCount = fc;
            const size_t kf = (size_t)bc * fc;
            clip.positions.resize(kf);
            clip.rotations.resize(kf);
            clip.scales.resize(kf);
            std::memcpy(clip.positions.data(), src, kf * 3 * sizeof(float)); src += kf * 3 * sizeof(float);
            std::memcpy(clip.rotations.data(), src, kf * 4 * sizeof(float)); src += kf * 4 * sizeof(float);
            std::memcpy(clip.scales.data(),    src, kf * 3 * sizeof(float)); src += kf * 3 * sizeof(float);
        }

        // per-mesh blend data
        out.meshBlends.resize(payloadMBC);
        for (uint32_t m = 0; m < payloadMBC; ++m)
        {
            uint32_t meshIdx = 0, vc = 0;
            std::memcpy(&meshIdx, src, sizeof(uint32_t)); src += sizeof(uint32_t);
            std::memcpy(&vc,      src, sizeof(uint32_t)); src += sizeof(uint32_t);
            out.meshBlends[m].meshIndex = meshIdx;
            out.meshBlends[m].blendData.resize(vc);
            std::memcpy(out.meshBlends[m].blendData.data(), src, vc * sizeof(BlendVertex));
            src += vc * sizeof(BlendVertex);
        }

        // Extended PMX data (ISKEL_FLAG_EXTENDED)
        const uint32_t iskelFlags = meta->reserved[1];
        if (iskelFlags & Resource::ISKEL_FLAG_EXTENDED)
        {
            // grantSource
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.grantSource[i], src, sizeof(int32_t)); src += sizeof(int32_t); }
            // grantRatio
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.grantRatio[i], src, sizeof(float)); src += sizeof(float); }
            // transformOrder
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(&sk.transformOrder[i], src, sizeof(int32_t)); src += sizeof(int32_t); }

            // IK chains
            uint32_t ikCount = 0;
            std::memcpy(&ikCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
            sk.ikChains.resize(ikCount);

            for (uint32_t ci = 0; ci < ikCount; ++ci)
            {
                auto& chain = sk.ikChains[ci];
                std::memcpy(&chain.ikBoneIndex,     src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.targetBoneIndex, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.loopCount,       src, sizeof(uint32_t)); src += sizeof(uint32_t);
                std::memcpy(&chain.angleLimit,      src, sizeof(float));    src += sizeof(float);

                uint32_t linkCount = 0;
                std::memcpy(&linkCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                chain.links.resize(linkCount);

                for (uint32_t li = 0; li < linkCount; ++li)
                {
                    auto& link = chain.links[li];
                    std::memcpy(&link.boneIndex, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                    uint8_t hasLim = *src++;
                    link.hasAngleLimit = (hasLim != 0);
                    if (link.hasAngleLimit)
                    {
                        std::memcpy(&link.minAngle, src, sizeof(DirectX::XMFLOAT3)); src += sizeof(DirectX::XMFLOAT3);
                        std::memcpy(&link.maxAngle, src, sizeof(DirectX::XMFLOAT3)); src += sizeof(DirectX::XMFLOAT3);
                    }
                }
            }

            // Morph target names. The file's count is how many 64-byte names
            // FOLLOW, and every one of them has to be stepped over even when
            // only the first MAX_MORPH_TARGETS are kept: the humanoid profile
            // is tail-appended right after this list, so stopping short reads
            // it from inside the name table.
            if (src + sizeof(uint32_t) <= dataEnd)
            {
                uint32_t fileMorphCount = 0;
                std::memcpy(&fileMorphCount, src, sizeof(uint32_t)); src += sizeof(uint32_t);
                const size_t fileNameBytes = static_cast<size_t>(fileMorphCount) * 64;
                if (fileNameBytes > static_cast<size_t>(dataEnd - src))
                {
                    LOG_ERROR("SceneInstanceLoader: .iskel '%s' declares %u morph names but only "
                              "%zu bytes remain - morph names dropped",
                              iskelPath.c_str(), fileMorphCount, static_cast<size_t>(dataEnd - src));
                    sk.morphTargetCount = 0;
                    src = dataEnd;
                }
                else
                {
                    sk.morphTargetCount = fileMorphCount;
                    if (sk.morphTargetCount > SkeletonAsset::MAX_MORPH_TARGETS)
                    {
                        LOG_WARNING("SceneInstanceLoader: .iskel '%s' has %u morph names; keeping the "
                                    "first %u (ENGINE_MAX_MORPH_TARGETS)",
                                    iskelPath.c_str(), fileMorphCount,
                                    static_cast<uint32_t>(SkeletonAsset::MAX_MORPH_TARGETS));
                        sk.morphTargetCount = SkeletonAsset::MAX_MORPH_TARGETS;
                    }
                    for (uint32_t i = 0; i < sk.morphTargetCount; ++i)
                        std::memcpy(sk.morphTargetNames[i], src + static_cast<size_t>(i) * 64, 64);
                    src += fileNameBytes;   // ALL names, kept or not
                }
            }

            uint32_t grantCount = 0;
            for (uint32_t i = 0; i < boneCount; ++i)
                if (sk.grantSource[i] >= 0) ++grantCount;
            LOG_INFO("SceneInstanceLoader: extended data — %u grants, %u IK chains, %u morphTargets",
                     grantCount, ikCount, sk.morphTargetCount);
        }
        else
        {
            // No extended block: rebuild the grant table from bone names.
            Resource::SkeletonImporter::BuildGrantTable(sk);
        }

        // Humanoid profile (retarget role map), tail-appended and gated by its flag.
        // Always last in the payload, so it reads correctly with or without the
        // extended PMX block before it.
        if (iskelFlags & Resource::ISKEL_FLAG_HUMANOID)
        {
            if (src + sizeof(HumanoidProfile) <= dataEnd)
            {
                std::memcpy(&sk.humanoid, src, sizeof(HumanoidProfile)); src += sizeof(HumanoidProfile);
                LOG_INFO("SceneInstanceLoader: humanoid profile — convention=%u, valid=%u",
                         sk.humanoid.convention, sk.humanoid.valid);
            }
            else
            {
                LOG_WARNING("SceneInstanceLoader: .iskel '%s' sets HUMANOID flag but payload is truncated",
                            iskelPath.c_str());
            }
        }

        out.valid = true;
        LOG_INFO("SceneInstanceLoader: .iskel '%s' — %u bones, %u clips, %u skinned meshes",
                 iskelPath.c_str(), boneCount, clipCount, payloadMBC);
        return out;
    }

    ImshStreams DecomposeImsh(const std::string& imshPath)
    {
        ImshStreams out;
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(imshPath, blob)) return out;
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
            return out;

        const Resource::MeshMetadata* meta =
            Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
        const uint32_t nv = meta->vertexCount;
        const uint32_t ni = meta->indexCount;

        const uint8_t* payload = Resource::GetPayload(blob.data());
        const auto* verts      = reinterpret_cast<const PackedVertex*>(payload);
        const auto* idxData    = reinterpret_cast<const uint32_t*>(payload + nv * sizeof(PackedVertex));

        out.positions.resize(nv);
        out.normals.resize(nv);
        out.uvs.resize(nv);
        for (uint32_t i = 0; i < nv; ++i)
        {
            out.positions[i] = { verts[i].px, verts[i].py, verts[i].pz };
            out.normals[i]   = { verts[i].nx, verts[i].ny, verts[i].nz };
            out.uvs[i]       = { verts[i].u,  verts[i].v  };
        }
        out.indices.assign(idxData, idxData + ni);
        out.valid = true;
        return out;
    }

    // Extract the vertex/index slice of one MeshLibraryEntry from an already-read
    // .meshlib blob, indices re-based into the local [0, vertexCount) range so the
    // streams are self-contained (the .imsh semantics SkinnedMeshPending expects).
    // Takes the blob so callers can cache it across a loop: a character's N skinned
    // parts usually share one .meshlib, and re-reading it per entry is
    // O(N x fileSize) (~330 ms for one character).
    ImshStreams DecomposeMeshLibEntryFromBlob(const std::vector<uint8_t>& blob, uint32_t meshId)
    {
        ImshStreams out;
        if (blob.empty()) return out;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
            return out;

        const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
        if (meshId >= meta->meshCount) return out;

        const uint8_t* payload   = Resource::GetPayload(blob.data());
        const auto*    entries   = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
        const auto&    entry     = entries[meshId];

        // Stride-aware: step by the on-disk vertexStride (32 without, 48 with
        // tangents), not sizeof(PackedVertex), so each pos/normal/uv triplet is found
        // whether or not a tangent follows it.
        const uint32_t vStride   = meta->vertexStride;
        const uint8_t* vbBase    = payload + meta->meshCount * sizeof(Resource::MeshLibraryEntry);
        const uint8_t* ibBase    = vbBase + size_t(meta->vertexCount) * vStride;
        const uint8_t* vertsBase = vbBase + size_t(entry.vertexStart) * vStride;
        const auto*    idxData   = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;

        // Second UV set (e.g. SDF face / lightmap) at its flag-driven offset; without
        // it the GBuffer descriptor's uv1 stream stays empty and FETCH_UV1 returns (0,0)
        // for every skinned character.
        const Resource::MeshLibVertexLayout L = Resource::ComputeMeshLibVertexLayout(meta->flags);
        const bool hasUV1 = (L.uv1Offset != 0xFFFFFFFFu);
        const bool hasUV2 = (L.uv2Offset != 0xFFFFFFFFu);
        const bool hasUV3 = (L.uv3Offset != 0xFFFFFFFFu);
        // The tangent at its flag-driven offset, like uv1. Nothing else fills
        // SkinnedMeshPending::tangents: without it a skinned mesh reaches GBuffer.vs with
        // tangent.bufferIndex == INVALID_BUFFER and takes the synthesised
        // normalize(cross(worldUpIsh, N)) frame, unrelated to UV space and swinging per
        // triangle, which breaks every tangent-space normal map (e.g. the Endfield hair
        // highlight built from its _HN "H Normal").
        const bool hasTan = (L.tangentOffset != 0xFFFFFFFFu);

        out.positions.resize(entry.vertexCount);
        out.normals.resize(entry.vertexCount);
        out.uvs.resize(entry.vertexCount);
        if (hasUV1) out.uvs1.resize(entry.vertexCount);
        if (hasUV2) out.uvs2.resize(entry.vertexCount);
        if (hasUV3) out.uvs3.resize(entry.vertexCount);
        if (hasTan) out.tangents.resize(entry.vertexCount);
        for (uint32_t i = 0; i < entry.vertexCount; ++i)
        {
            const uint8_t* vp = vertsBase + size_t(i) * vStride;
            const auto* v = reinterpret_cast<const PackedVertex*>(vp);
            out.positions[i] = { v->px, v->py, v->pz };
            out.normals[i]   = { v->nx, v->ny, v->nz };
            out.uvs[i]       = { v->u,  v->v  };
            if (hasUV1)
            {
                const float* u1 = reinterpret_cast<const float*>(vp + L.uv1Offset);
                out.uvs1[i] = { u1[0], u1[1] };
            }
            if (hasUV2)
            {
                const float* u1 = reinterpret_cast<const float*>(vp + L.uv2Offset);
                out.uvs2[i] = { u1[0], u1[1] };
            }
            if (hasUV3)
            {
                const float* u1 = reinterpret_cast<const float*>(vp + L.uv3Offset);
                out.uvs3[i] = { u1[0], u1[1] };
            }
            if (hasTan)
            {
                const float* t = reinterpret_cast<const float*>(vp + L.tangentOffset);
                out.tangents[i] = { t[0], t[1], t[2], t[3] };
            }
        }
        // Re-base indices into local [0, vertexCount): the meshlib stores them offset
        // into the shared VB; SkinnedMeshPending expects local indices.
        out.indices.resize(entry.indexCount);
        for (uint32_t i = 0; i < entry.indexCount; ++i)
            out.indices[i] = idxData[i] - entry.vertexStart;
        out.valid = true;
        return out;
    }

    // Thin wrapper: read the whole .meshlib then extract one entry. Used by the
    // uncached single-shot path; the skinned loop passes a blob cache through
    // DecomposeMeshSource to avoid re-reading shared .meshlib files.
    ImshStreams DecomposeMeshLibEntry(const std::string& meshlibPath, uint32_t meshId)
    {
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(meshlibPath, blob)) return {};
        return DecomposeMeshLibEntryFromBlob(blob, meshId);
    }

    // File-extension dispatcher: picks the decoder for a .imsh or a .meshlib
    // entry. @p meshlibBlobCache (optional) dedupes whole-file .meshlib reads across
    // a loop; pass it when decoding many entries from shared libs.
    ImshStreams DecomposeMeshSource(const std::string& path, uint32_t meshId,
                                    std::unordered_map<std::string, std::vector<uint8_t>>* meshlibBlobCache = nullptr)
    {
        const auto dot = path.find_last_of('.');
        if (dot != std::string::npos)
        {
            std::string ext = path.substr(dot);
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".meshlib")
            {
                if (meshlibBlobCache)
                {
                    auto it = meshlibBlobCache->find(path);
                    if (it == meshlibBlobCache->end())
                    {
                        std::vector<uint8_t> b;
                        ::Resource::AssetFS::Get().ReadFile(path, b);   // read ONCE per unique .meshlib
                        it = meshlibBlobCache->emplace(path, std::move(b)).first;
                    }
                    return DecomposeMeshLibEntryFromBlob(it->second, meshId);
                }
                return DecomposeMeshLibEntry(path, meshId);
            }
        }
        return DecomposeImsh(path);
    }

    // RegistrationReuseScope: the registrations loads under such a scope made, for
    // the process (registry indices are never invalidated). Key: the share key + the
    // .iskel's size and write time.
    std::unordered_map<std::string, ReusedRegistration>& ReusedRegistrations()
    {
        static std::unordered_map<std::string, ReusedRegistration> s_registrations;
        return s_registrations;
    }
    std::string FileStampKey(const std::string& path)
    {
        std::error_code ec;
        // The file the read resolves to (a worktree's shared content root too).
        const std::string onDisk = ::Resource::AssetFS::Get().ResolveOnDisk(path);
        const auto p    = Sys::U8Path(onDisk.empty() ? path : onDisk);
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) return {};
        const auto when = std::filesystem::last_write_time(p, ec);
        if (ec) return {};
        return std::to_string(size) + ":" + std::to_string(when.time_since_epoch().count());
    }

    // The runtime spawn template's skeleton registrations: one per (.iskel + its VB
    // records + the .iskel's stamp) for the process (registry indices are never
    // invalidated), with the model's skinned meshes, so a spawn whose every skinned
    // part finds its shared source live reads nothing at all. Separate from the
    // RegistrationReuseScope map above: only runtime spawns use it, and a placed
    // model never borrows from or lends to it.
    std::unordered_map<std::string, TemplateRegistration>& TemplateRegistrations()
    {
        static std::unordered_map<std::string, TemplateRegistration> s_registrations;
        return s_registrations;
    }

    std::size_t StreamsBytes(const ImshStreams& s)
    {
        return s.positions.size() * sizeof(DirectX::XMFLOAT3)
             + s.normals.size()   * sizeof(DirectX::XMFLOAT3)
             + s.uvs.size()       * sizeof(DirectX::XMFLOAT2)
             + s.uvs1.size()      * sizeof(DirectX::XMFLOAT2)
             + s.uvs2.size()      * sizeof(DirectX::XMFLOAT2)
             + s.uvs3.size()      * sizeof(DirectX::XMFLOAT2)
             + s.tangents.size()  * sizeof(DirectX::XMFLOAT4)
             + s.indices.size()   * sizeof(uint32_t);
    }

    std::string CacheKey(const std::string& path, uint32_t id)
    {
        return path + '#' + std::to_string(id);
    }

    // Decode (path, meshId). With a BatchScope open the result is memoised and
    // the returned pointer aliases the cache; otherwise it aliases @p scratch.
    // Never null: an undecodable source yields streams with valid == false.
    const ImshStreams* DecodeMeshSourceCached(
        const std::string& path, uint32_t meshId, ImshStreams& scratch,
        std::unordered_map<std::string, std::vector<uint8_t>>* blobCache)
    {
        PROF_SCOPE("SIL.DecodeMesh");
        if (!g_batch)
        {
            scratch = DecomposeMeshSource(path, meshId, blobCache);
            return &scratch;
        }

        const std::string key = CacheKey(path, meshId);
        auto it = g_batch->streams.find(key);
        if (it != g_batch->streams.end())
        {
            ++g_batch->streamHit;
            return &it->second;
        }

        ++g_batch->streamMiss;
        scratch = DecomposeMeshSource(path, meshId, blobCache);
        const std::size_t bytes = StreamsBytes(scratch);
        if (g_batch->decodedBytes + bytes > kBatchCacheBudgetBytes)
            return &scratch;                       // over budget: decode, don't retain

        g_batch->decodedBytes += bytes;
        return &g_batch->streams.emplace(key, scratch).first->second;
    }

    bool PrepareModel(const std::string& iscnPath, ModelPrep& out)
    {
        std::vector<uint8_t> blob;
        if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob))
        {
            LOG_ERROR("SceneInstanceLoader: cannot open '%s'", iscnPath.c_str());
            return false;
        }
        if (blob.empty()) return false;

        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
        {
            LOG_ERROR("SceneInstanceLoader: bad .iscn header in '%s'", iscnPath.c_str());
            return false;
        }

        // Text payload, not necessarily NUL-terminated: SceneImporter and
        // UnrealSceneImporter write dataSize = textLength + 1 with one trailing NUL, a
        // PmxImporter file may carry dataSize = textLength and no terminator, so a string
        // built from the bare pointer would strlen() off the end of the vector. Take
        // exactly dataSize bytes, stopped at the first NUL if there is one.
        const std::string text = [&]() -> std::string
        {
            const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
            const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
            const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
            size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
            const void*  nul  = std::memchr(payl, 0, n);
            if (nul) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
            return std::string(payl, n);
        }();

        // The model's asset collision (its BO / PR records).
        {
            std::vector<std::string> warns;
            Resource::ParseModelCollision(text, out.modelCollision, &warns);
            for (const std::string& w : warns)
                LOG_WARNING("SceneInstanceLoader: '%s' asset collision: %s", iscnPath.c_str(), w.c_str());
        }

        // The model's sockets (its SO records), derived onto the skeleton root once the
        // skeleton is attached (the end of Load).
        {
            std::vector<std::string> warns;
            Resource::ParseModelSockets(text, out.modelSockets, &warns);
            for (const std::string& w : warns)
                LOG_WARNING("SceneInstanceLoader: '%s' sockets: %s", iscnPath.c_str(), w.c_str());
        }
        // The model's virtual bones (its VB records), appended to its skeleton before
        // it is registered.
        {
            std::vector<std::string> warns;
            Resource::ParseModelVirtualBones(text, out.modelVirtualBones, &warns);
            for (const std::string& w : warns)
                LOG_WARNING("SceneInstanceLoader: '%s' virtual bones: %s", iscnPath.c_str(), w.c_str());
        }

        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.size() < 2) continue;
            if (line[0] == '#') continue;

            if (line[0] == 'N' && line[1] == ' ')
            {
                NodeRecord nr;
                if (ParseNLine(line, nr))
                    out.nodes.push_back(std::move(nr));
            }
            else if (line[0] == 'F' && line[1] == ' ')
            {
                FileRecord fr;
                if (ParseFLine(line, fr))
                    out.fileRecords.push_back(std::move(fr));
            }
            else if (line[0] == 'C' && line[1] == ' ')
            {
                CollisionRecord cr;
                if (ParseCLine(line, cr) && cr.fileIdx >= 0)
                    out.collisionRecords[cr.fileIdx].push_back(std::move(cr));
            }
            else if (line[0] == 'V' && line[1] == ' ')
            {
                CollisionRecord cr;
                if (ParseCLine(line, cr) && cr.nodeIdx >= 0)
                    out.volumeRecords[cr.nodeIdx].push_back(std::move(cr));
            }
            else if (line[0] == 'K' && line[1] == ' ')
            {
                ParseKLine(line, out.skelRelFile);
            }
            else if (line[0] == 'M' && line[1] == ' ')
            {
                // M idx=<meshFileIdx> file=<morphFileName>
                MorphFileRef mref;
                mref.meshFileIdx = -1;
                auto idxPos = line.find("idx=");
                auto filePos = line.find("file=");
                if (idxPos != std::string::npos)
                    mref.meshFileIdx = std::stoi(line.substr(idxPos + 4));
                if (filePos != std::string::npos)
                    mref.file = line.substr(filePos + 5);
                // Trim trailing whitespace
                while (!mref.file.empty() && (mref.file.back() == ' ' || mref.file.back() == '\r' || mref.file.back() == '\n'))
                    mref.file.pop_back();
                if (mref.meshFileIdx >= 0 && !mref.file.empty())
                    out.morphFileRefs.push_back(std::move(mref));
            }
        }

        if (out.nodes.empty())
        {
            LOG_ERROR("SceneInstanceLoader: no nodes found in '%s'", iscnPath.c_str());
            return false;
        }

        // Sort nodes by idx so parent entities are always created first.
        std::sort(out.nodes.begin(), out.nodes.end(),
                  [](const NodeRecord& a, const NodeRecord& b){ return a.idx < b.idx; });
        return true;
    }

    // A runtime spawn's model records (Resource/PrefabTemplate.h), kept for the
    // process while the file's stamp holds; filled by the first spawn or by a
    // preload. Main thread.
    std::unordered_map<std::string, CachedSpawnPrep>& SpawnPreps()
    {
        static std::unordered_map<std::string, CachedSpawnPrep> s_preps;
        return s_preps;
    }

    // nullptr = the file does not prepare.
    std::shared_ptr<const ModelPrep> PrepareModelForSpawn(const std::string& iscnPath)
    {
        using Cached = CachedSpawnPrep;
        auto& s_preps = SpawnPreps();
        const std::string stamp = Resource::PrefabTemplate::FileStamp(iscnPath);
        Cached& slot = s_preps[iscnPath];
        if (slot.prep && !stamp.empty() && slot.stamp == stamp)
        {
            ++Resource::PrefabTemplate::Stats().fileHits;
            return slot.prep;
        }
        auto prep = std::make_shared<ModelPrep>();
        if (!PrepareModel(iscnPath, *prep))
        {
            slot = Cached{};
            return nullptr;
        }
        ++Resource::PrefabTemplate::Stats().fileMisses;
        slot = Cached{ stamp, prep };
        return prep;
    }

    // Content identity of one skinned part's source streams
    // (SkinnedMeshPending::sourceKey): the mesh source + its entry id, the .iskel the
    // blend data came from, the mesh-file index that selects the morph block, the
    // morph files, and each file's stamp, so a re-import is a new key instead of a
    // borrow of the old upload (m_sharedSources lives until the world clears). Equal
    // keys are byte-identical by construction. One function for Load and the
    // preload: a preloaded source is found only under the key the spawn computes.
    template <typename StampOf>
    uint64_t PartKeyOf(const std::string& meshAbsPath, uint32_t meshId, const std::string& iskelPath, int fi,
                       const std::vector<MorphFileRef>& morphFileRefs, const fs::path& iscnDir, StampOf&& stampOf)
    {
        uint64_t h = 1469598103934665603ull;      // FNV-1a 64
        auto mix = [&h](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i)
            { h ^= b[i]; h *= 1099511628211ull; }
        };
        auto mixStr = [&mix](const std::string& v) { mix(v.data(), v.size()); };
        mixStr(meshAbsPath);
        mix(&meshId, sizeof(uint32_t));
        mixStr(iskelPath);
        mix(&fi, sizeof(int));
        // The morph block is part of the shared buffers too
        // (morphDelta), and it is chosen by the M-line's FILE,
        // not by anything above: two models that share a
        // .meshlib + .iskel but carry different .imorph files
        // must not borrow each other's deltas.
        for (const auto& mref : morphFileRefs)
        {
            if (mref.meshFileIdx != fi) continue;
            const std::string imorphKeyPath =
                Sys::PathToUtf8(iscnDir / Sys::U8Path(mref.file));
            mixStr(imorphKeyPath);
            mixStr(stampOf(imorphKeyPath));
        }
        mixStr(stampOf(meshAbsPath));
        mixStr(stampOf(iskelPath));
        return h ? h : 1ull;                      // 0 means "unique"
    }

} // anonymous namespace

// ==== SceneInstanceLoader::BatchScope ====
// Flat depth counter: the outermost scope owns the cache and the upload batch
// (the nesting rule of BeginBufferUploadBatch / EndBufferUploadBatch).
SceneInstanceLoader::BatchScope::BatchScope(IGraphicsDevice* gfx)
{
    if (g_batchDepth++ == 0)
    {
        g_batch = new BatchCache();

        // Only the outermost scope owns the upload batch, so a nested scope
        // (a prefab instantiated during a world load) cannot close it early
        // and bring back a drain per character.
        m_gfx = gfx;
        if (m_gfx)
        {
            m_gfx->BeginBufferUploadBatch();
            m_gfx->BeginBufferHeapBatch();
        }
    }
}

SceneInstanceLoader::BatchScope::~BatchScope()
{
    if (m_gfx)
    {
        PROF_SCOPE("SIL.BatchEnd");   // the batch's one flush + its staging release
        m_gfx->EndBufferHeapBatch();
        m_gfx->EndBufferUploadBatch();   // the ONE FlushUploadAndWait for the batch
        m_gfx = nullptr;
    }
    if (--g_batchDepth > 0) return;
    if (g_batch)
    {
        if (g_batch->streamHit || g_batch->morphHit || g_batch->iskelHit)
            LOG_INFO("[LOAD-PROFILE] SceneInstanceLoader batch cache: "
                     "mesh %u hit / %u decode, morph %u hit / %u extract, "
                     "iskel %u hit / %u load (skeletons NOT duplicated: saved %u x %zu KB), "
                     "%.1f MB retained",
                     g_batch->streamHit, g_batch->streamMiss,
                     g_batch->morphHit,  g_batch->morphMiss,
                     g_batch->iskelHit,  g_batch->iskelMiss,
                     g_batch->iskelHit,  sizeof(SkeletonAsset) / 1024,
                     double(g_batch->decodedBytes) / (1024.0 * 1024.0));
        delete g_batch;
        g_batch = nullptr;
    }
    g_batchDepth = 0;   // defensive: never let an unbalanced dtor go negative
}

SceneInstanceLoader::RegistrationReuseScope::RegistrationReuseScope() { ++g_reuseDepth; }
SceneInstanceLoader::RegistrationReuseScope::~RegistrationReuseScope()
{
    if (g_reuseDepth > 0) --g_reuseDepth;
}

// Read-only preview: the .iskel's skeleton and, per blend stream, the positions
// and indices of @p meshPath's entry with its weights. Fails when an entry does
// not decode or its vertex count differs from its blend stream.
bool SceneInstanceLoader::LoadPreviewMeshes(const std::string& meshPath,const std::string& skeletonPath,
    SkeletonAsset& skeleton,std::vector<PreviewMeshData>& meshes)
{
    auto data=LoadIskel(skeletonPath);
    if(!data.valid||!data.skeleton)return false;
    skeleton=std::move(*data.skeleton); meshes.clear();
    std::unordered_map<std::string,std::vector<uint8_t>> blobs;
    for(const auto& blend:data.meshBlends) {
        ImshStreams scratch;
        const auto* source=DecodeMeshSourceCached(meshPath,blend.meshIndex,scratch,&blobs);
        if(!source||!source->valid||source->positions.size()!=blend.blendData.size())return false;
        PreviewMeshData mesh;mesh.positions=source->positions;mesh.indices=source->indices;mesh.weights=blend.blendData;
        meshes.push_back(std::move(mesh));
    }
    return !meshes.empty();
}

bool SceneInstanceLoader::LoadSkeletonFile(const std::string&      iskelPath,
                                           SkeletonAsset&          outSkeleton,
                                           std::vector<ClipAsset>& outClips)
{
    return LoadSkeletonFile(iskelPath, outSkeleton, outClips, nullptr);
}

bool SceneInstanceLoader::LoadSkeletonFile(const std::string&            iskelPath,
                                           SkeletonAsset&                outSkeleton,
                                           std::vector<ClipAsset>&       outClips,
                                           std::vector<SkinBlendStream>* outBlends)
{
    IskelData d = LoadIskel(iskelPath);
    if (!d.valid || !d.skeleton) return false;
    outSkeleton = std::move(*d.skeleton);
    outClips    = std::move(d.clips);
    if (outBlends)
    {
        outBlends->clear();
        outBlends->reserve(d.meshBlends.size());
        for (IskelMeshBlend& mb : d.meshBlends)
            outBlends->push_back(SkinBlendStream{ mb.meshIndex, std::move(mb.blendData) });
    }
    return true;
}

// The model's K line, read through AssetFS so a packed model answers too;
// silent on a miss.
std::string SceneInstanceLoader::ModelSkeletonPath(const std::string& iscnPath)
{
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob) ||
        !Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
        return {};
    const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
    const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
    const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
    if (off > blob.size()) return {};
    size_t n = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
    if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
    for (const FileReference& r : ListFileReferences(std::string(payl, n), iscnPath))
        if (r.kind == 'K') return r.path;
    return {};
}

// ==== SceneInstanceLoader::Load ====
SceneInstanceLoader::LoadResult SceneInstanceLoader::Load(const std::string&      iscnPath,
                                                           World&                  world,
                                                           Resource::AssetManager& assetMgr,
                                                           Resource::MeshLibrary&  meshLib,
                                                           Renderer*               renderer)
{
    LoadResult result;

    // [LOAD-PROFILE] per-phase CPU timing. Load is the dominant cost of a world load
    // (one Bistro .iscn: 1306 nodes, 1591 meshes, 132 materials); the breakdown
    // separates the pure-CPU phases (read+parse, DecomposeMeshSource, .imat parse)
    // from the main-thread-only ones (ECS mutation, GPU upload).
    const auto _t0 = std::chrono::steady_clock::now();
    auto _ms = [](auto a, auto b){ return std::chrono::duration<double, std::milli>(b - a).count(); };
    // The same phases on the engine profiler's timeline.
    static ::Diag::Prof::NameSlot s_stRead{ "SIL.ReadParse" }, s_stMesh{ "SIL.MeshLib" },
        s_stPre{ "SIL.PreSpawn" }, s_stSpawn{ "SIL.Spawn" }, s_stSkin{ "SIL.Skinned" },
        s_stTail{ "SIL.Tail" };
    ::Diag::Prof::StageChain stage;
    stage.Next(s_stRead);

    IGraphicsDevice* gfxPtr = assetMgr.GetGraphicsDevice();
    if (!gfxPtr)
    {
        LOG_ERROR("SceneInstanceLoader: AssetManager has no graphics device; cannot load '%s'",
                  iscnPath.c_str());
        return result;
    }
    IGraphicsDevice& gfx = *gfxPtr;

    // ---- Prepare: the model file's records (pure data) ----
    // A runtime spawn takes them from its template (Resource/PrefabTemplate.h,
    // validated by the file's stamp); every other load reads and parses the file.
    std::shared_ptr<const ModelPrep> prepHeld;
    ModelPrep                        prepOwn;
    const ModelPrep*                 prep = nullptr;
    if (Resource::PrefabTemplate::Active())
        prep = (prepHeld = PrepareModelForSpawn(iscnPath)).get();
    else if (PrepareModel(iscnPath, prepOwn))
        prep = &prepOwn;
    if (!prep) return result;

    const std::vector<NodeRecord>& nodes       = prep->nodes;
    const std::vector<FileRecord>& fileRecords = prep->fileRecords;
    const std::unordered_map<int, std::vector<CollisionRecord>>& collisionRecords = prep->collisionRecords;
    const std::unordered_map<int, std::vector<CollisionRecord>>& volumeRecords    = prep->volumeRecords;
    const std::string&                 skelRelFile       = prep->skelRelFile;
    const std::vector<MorphFileRef>&   morphFileRefs     = prep->morphFileRefs;
    const Resource::ModelCollision&    modelCollision    = prep->modelCollision;
    const Resource::ModelSockets&      modelSockets      = prep->modelSockets;
    const Resource::ModelVirtualBones& modelVirtualBones = prep->modelVirtualBones;

    LOG_INFO("SceneInstanceLoader: '%s' — %zu nodes, %zu mesh files",
             iscnPath.c_str(), nodes.size(), fileRecords.size());

    // ---- Acquire mesh libraries ----
    // Each distinct `file=` of an F-line is one .meshlib, loaded once and uploaded as
    // a single shared VB + IB; the F-lines sharing it look their mesh up by meshId.
    const fs::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    std::unordered_map<std::string, Resource::Handle> libByPath;
    libByPath.reserve(8);

    // Per-F-line resolved data: which library + which meshId + AABB + counts.
    std::vector<Resource::Handle>  fiLibHandle(fileRecords.size());
    std::vector<uint32_t>          fiMeshId      (fileRecords.size(), 0u);
    std::vector<DirectX::XMFLOAT3> fiAabbMin     (fileRecords.size(), DirectX::XMFLOAT3{0,0,0});
    std::vector<DirectX::XMFLOAT3> fiAabbMax     (fileRecords.size(), DirectX::XMFLOAT3{0,0,0});
    std::vector<uint32_t>          fiIndexCount  (fileRecords.size(), 0u);
    std::vector<uint32_t>          fiVertexCount (fileRecords.size(), 0u);
    std::vector<std::string>       meshAbsPaths  (fileRecords.size());

    const auto _tParse = std::chrono::steady_clock::now();   // .iscn read + parse done

    uint32_t failedCount = 0;
    uint32_t keysMoved = 0, keysMissing = 0;   // sec= key vs the mesh= hint
    // Batch all per-library VB/IB uploads: each MeshLibrary::Load issues 2
    // CreateBuffer(DEFAULT) calls, each a synchronous flush outside a batch
    // (Close->Execute->wait->Reset on the COPY queue). The batch coalesces them
    // (mid-batch auto-flush capped by kBatchFlushBytes/Count), turning 2*N waits
    // into a handful. EndBufferUploadBatch waits on the upload context's own fences,
    // not WaitForPreviousFrame, so this is safe in any frame phase.
    stage.Next(s_stMesh);
    gfx.BeginBufferUploadBatch();
    for (int fi = 0; fi < static_cast<int>(fileRecords.size()); ++fi)
    {
        const auto& fr       = fileRecords[fi];
        meshAbsPaths[fi]     = Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file));

        auto it = libByPath.find(meshAbsPaths[fi]);
        Resource::Handle libH;
        if (it != libByPath.end())
        {
            libH = it->second;
        }
        else
        {
            libH = meshLib.Load(meshAbsPaths[fi], gfx);
            libByPath.emplace(meshAbsPaths[fi], libH);
            // Hand the freshly loaded library to the Renderer so the next
            // OnWorldClear releases it; otherwise every world reload leaks one
            // VB+IB pair per referenced .meshlib (MeshLibrary::Load does not
            // dedupe by path).
            if (renderer && libH.IsValid())
                renderer->TrackWorldMeshLibrary(libH);
        }

        if (!libH.IsValid())
        {
            ++failedCount;
            continue;
        }

        // The section KEY names the entry; `mesh=` is the hint a binary without key
        // support reads. A re-imported library that reordered its entries moves the
        // key's entry and leaves the hint stale.
        uint32_t entryId = fr.subIdx;
        if (fr.sectionKey != 0)
        {
            const int byKey = meshLib.FindEntryBySectionKey(libH, fr.sectionKey);
            if (byKey < 0)
                ++keysMissing;
            else if (static_cast<uint32_t>(byKey) != entryId)
            {
                ++keysMoved;
                entryId = static_cast<uint32_t>(byKey);
            }
        }

        const auto* entry = meshLib.GetEntry(libH, entryId);
        if (!entry)
        {
            LOG_WARNING("SceneInstanceLoader: F[%d] meshId=%u out of range in '%s'",
                        fi, entryId, fr.file.c_str());
            ++failedCount;
            continue;
        }

        fiLibHandle[fi]   = libH;
        fiMeshId[fi]      = entryId;
        fiAabbMin[fi]     = entry->aabbMin;
        fiAabbMax[fi]     = entry->aabbMax;
        fiIndexCount[fi]  = entry->indexCount;
        fiVertexCount[fi] = entry->vertexCount;
    }
    gfx.EndBufferUploadBatch();
    const auto _tMeshLib = std::chrono::steady_clock::now();  // mesh-library load + GPU upload done
    stage.Next(s_stPre);
    if (failedCount > 0)
        LOG_WARNING("SceneInstanceLoader: %u/%zu mesh refs failed to resolve",
                    failedCount, fileRecords.size());
    if (keysMoved > 0 || keysMissing > 0)
    {
        // The fix is the asset tool, which only a harness build carries: Retail
        // strips it (Engine.Cpp.props), and tools/strip_gate.py fails a Retail
        // binary that spells its flag. So only a harness build names it here.
#if WITH_HARNESS
        const char* refreshHint = "; -assettool=iscn_keys -apply refreshes the hints";
#else
        const char* refreshHint = "";
#endif
        LOG_WARNING("SceneInstanceLoader: '%s' - %u F-line(s) followed their section key to another "
                    "entry than their mesh= hint (the library was re-imported%s), %u key(s) the "
                    "library does not have (the hint was used)",
                    iscnPath.c_str(), keysMoved, refreshHint, keysMissing);
    }

    // ---- Build ECS hierarchy (DFS from root nodes) ----
    // Roots are the nodes whose parent == -1.
    std::unordered_map<int, Entity> entityMap;
    entityMap.reserve(nodes.size() * 2);

    // meshFileIdx -> all mesh entities created for that file
    const bool needSkelMap = (!skelRelFile.empty() && renderer != nullptr);
    std::unordered_map<int, std::vector<Entity>> meshFileToEntities;

    uint32_t nodeCount = 0, meshCount = 0, colliderCount = 0;
    Entity rootEntity = NullEntity;

    // Nodes are sorted by idx (PrepareModel), so the roots and each node's children
    // spawn in ascending idx, the order ReadMeshPlacements assumes.

    // Material cache shared across the whole scene tree (see the material load in
    // SpawnSingleNode for why it lives at Load scope).
    std::unordered_map<std::string, MaterialComponent> matCache;
    matCache.reserve(256);

    // Children adjacency built once: node idx -> array indices of its children, in
    // ascending array order (no O(N^2) parent scan per level).
    std::unordered_map<int, std::vector<int>> childrenByIdx;
    childrenByIdx.reserve(nodes.size());
    for (int k = 0; k < static_cast<int>(nodes.size()); ++k)
        if (nodes[k].parent >= 0)
            childrenByIdx[nodes[k].parent].push_back(k);

    // Iterative pre-order DFS, not recursion: SpawnSingleNode has a large stack
    // frame (two MaterialComponent and ~14 component temporaries, unpooled in a
    // /RTC1 Debug build), and recursing per hierarchy level on top of the deep ImGui
    // call stack overflowed the 1 MB stack (0xC00000FD) on deep skeletons (1733
    // nodes, depth 70). An explicit work stack keeps the call depth O(1); `spawned`
    // guards against malformed cyclic input (a node reachable more than once).
    std::vector<bool> spawned(nodes.size(), false);
    std::vector<int>  dfs;
    dfs.reserve(nodes.size());

    // Per F-line, its section's key and the key of the Mesh owning it, from the
    // library identity (its own, or the one synthesized at load).
    AssetCollisionSpawn assetCollision;
    if (!modelCollision.meshes.empty())
    {
        assetCollision.collision = &modelCollision;
        assetCollision.model     = Sys::RelativizeAssetPath(iscnPath);
        assetCollision.fiSectionKey.assign(fileRecords.size(), 0);
        assetCollision.fiMeshKey.assign(fileRecords.size(), 0);
        for (size_t fi = 0; fi < fileRecords.size(); ++fi)
        {
            if (!fiLibHandle[fi].IsValid()) continue;
            const auto id = meshLib.GetIdentity(fiLibHandle[fi]);
            if (!id || fiMeshId[fi] >= id->sections.size()) continue;
            const Resource::MeshIdentity::Section& s = id->sections[fiMeshId[fi]];
            assetCollision.fiSectionKey[fi] = s.key;
            if (s.ownerMesh < id->meshes.size()) assetCollision.fiMeshKey[fi] = id->meshes[s.ownerMesh].key;
        }
    }

    stage.Next(s_stSpawn);
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        if (nodes[i].parent != -1) continue;   // start a DFS from each root

        dfs.push_back(i);
        while (!dfs.empty())
        {
            const int ni = dfs.back();
            dfs.pop_back();
            if (ni < 0 || ni >= static_cast<int>(nodes.size()) || spawned[ni])
                continue;
            spawned[ni] = true;

            Entity e = SpawnSingleNode(ni, nodes,
                                       fiLibHandle, fiMeshId, fiIndexCount, fiVertexCount,
                                       fiAabbMin, fiAabbMax,
                                       meshAbsPaths, fileRecords, iscnDir,
                                       world, assetMgr, entityMap, matCache,
                                       nodeCount, meshCount,
                                       needSkelMap ? &meshFileToEntities : nullptr,
                                       collisionRecords.empty() ? nullptr : &collisionRecords,
                                       &colliderCount,
                                       volumeRecords.empty() ? nullptr : &volumeRecords,
                                       assetCollision.collision ? &assetCollision : nullptr);
            if (rootEntity == NullEntity)
                rootEntity = e;

            // Push children in reverse array order so they pop, and spawn,
            // in ascending array order: entity ids and sibling order follow
            // the array.
            auto it = childrenByIdx.find(nodes[ni].idx);
            if (it != childrenByIdx.end())
                for (auto r = it->second.rbegin(); r != it->second.rend(); ++r)
                    dfs.push_back(*r);
        }
    }

    const auto _tSpawn = std::chrono::steady_clock::now();   // SpawnSingleNode DFS + material loading done
    stage.Next(s_stSkin);

    // Sub-phase accounting for the [LOAD-PROFILE] line below: mesh decode, morph
    // extraction and GPU registration are timed apart.
    double _msDecompose = 0.0, _msMorph = 0.0, _msRegister = 0.0;
    double _msIskel = 0.0, _msRegistry = 0.0, _msPendingCopy = 0.0, _msUploadFlush = 0.0;
    auto   _now = [] { return std::chrono::steady_clock::now(); };

    // ---- Skeleton / skinned mesh registration ----
    if (needSkelMap)
    {
        const std::string iskelPath = Sys::PathToUtf8(iscnDir / Sys::U8Path(skelRelFile));
        // The skeleton this model registers is the .iskel plus its virtual
        // bones: two models sharing an .iskel with different VB records must
        // not share one asset.
        const uint64_t    vbSig    = Resource::VirtualBonesSignature(modelVirtualBones.bones);
        const std::string shareKey = vbSig ? iskelPath + "#vb" + Resource::MeshKeyToHex(vbSig) : iskelPath;

        // Content identity of one skinned part's source streams (PartKeyOf).
        // Each file is stat'ed once per BatchScope (a scene load of 100 instances
        // of a model asks once), else once per call.
        std::unordered_map<std::string, std::string> localStamps;
        auto& stamps = g_batch ? g_batch->stamps : localStamps;
        auto stampOf = [&stamps](const std::string& p) -> const std::string&
        {
            auto it = stamps.find(p);
            if (it == stamps.end())
                it = stamps.emplace(p, Resource::PrefabTemplate::FileStamp(p)).first;
            return it->second;
        };
        auto partKey = [&](int fi) -> uint64_t
        {
            return PartKeyOf(meshAbsPaths[fi], fiMeshId[fi], iskelPath, fi, morphFileRefs, iscnDir, stampOf);
        };

        // ---- The runtime spawn's template (Resource/PrefabTemplate.h) ----
        // Its registration of this skeleton, made by the first runtime spawn of
        // the model and reused by every later one. When every skinned part's
        // source is live in the renderer the spawn reads nothing (no .iskel, no
        // .meshlib decode, no .imorph) and each part takes the shared path with
        // no CPU streams. A key needs the .iskel's stamp: a file whose stamp is
        // unknown is never reused.
        const bool        tplActive = Resource::PrefabTemplate::Active() && renderer;
        const std::string tplKey    = (tplActive && !stampOf(iskelPath).empty())
                                    ? shareKey + "@" + stampOf(iskelPath) : std::string();
        const TemplateRegistration* treg = nullptr;
        if (!tplKey.empty())
        {
            const auto it = TemplateRegistrations().find(tplKey);
            if (it != TemplateRegistrations().end() &&
                it->second.skelIdx < renderer->GetSkeletonRegistry().Count())
                treg = &it->second;
        }
        bool sharedOnly = false;
        if (treg && g_batch && !treg->skinnedMeshes.empty())
        {
            sharedOnly = true;
            for (const uint32_t mi : treg->skinnedMeshes)
            {
                const int fi = static_cast<int>(mi);
                if (fi >= static_cast<int>(meshAbsPaths.size()) || meshAbsPaths[fi].empty() ||
                    !renderer->HasSharedSkinnedSource(partKey(fi)))
                {
                    sharedOnly = false;
                    break;
                }
            }
        }

        // Shared-skeleton fast path: a later instance of the same character
        // reuses the registry entry instead of re-reading, re-deserializing and
        // re-registering an identical SkeletonAsset (see IskelShare).
        IskelShare        localShare;            // owns the data when no batch is open
        const IskelShare* resolved = nullptr;

        if (g_batch)
        {
            auto it = g_batch->iskels.find(shareKey);
            if (it != g_batch->iskels.end()) { ++g_batch->iskelHit; resolved = &it->second; }
        }

        if (!resolved && sharedOnly)
        {
            // The template's registration and skinned meshes. A part that takes
            // the shared path needs no blend data, so none is read. Not put in
            // the batch cache: its blend lists are empty.
            localShare.skelIdx   = treg->skelIdx;
            localShare.clipCount = treg->clipCount;
            localShare.valid     = true;
            localShare.meshBlends.resize(treg->skinnedMeshes.size());
            for (size_t k = 0; k < treg->skinnedMeshes.size(); ++k)
                localShare.meshBlends[k].meshIndex = treg->skinnedMeshes[k];
            resolved = &localShare;
            ++Resource::PrefabTemplate::Stats().skeletonReuses;
            LOG_INFO("SceneInstanceLoader: '%s' reuses skeleton idx=%u (the runtime spawn template; registry stays "
                     "%u skeletons; every skinned part's source is shared, nothing read)",
                     iscnPath.c_str(), localShare.skelIdx, renderer->GetSkeletonRegistry().Count());
        }

        if (!resolved)
        {
            if (g_batch) ++g_batch->iskelMiss;

            const auto _tI0 = _now();
            IskelData d = LoadIskel(iskelPath);
            _msIskel = std::chrono::duration<double, std::milli>(_now() - _tI0).count();

            IskelShare fresh;
            // RegistrationReuseScope: the same skeleton registered by an earlier
            // scoped load (a preview stage's respawn) - its index, not another copy.
            std::string reuseKey;
            const ReusedRegistration* reused = nullptr;
            if (g_reuseDepth > 0 && renderer && d.valid && !d.meshBlends.empty())
            {
                const std::string stamp = FileStampKey(iskelPath);
                if (!stamp.empty())
                {
                    reuseKey = shareKey + "@" + stamp;
                    auto it  = ReusedRegistrations().find(reuseKey);
                    if (it != ReusedRegistrations().end() &&
                        it->second.skelIdx < renderer->GetSkeletonRegistry().Count())
                        reused = &it->second;
                }
            }
            if (reused)
            {
                fresh.skelIdx   = reused->skelIdx;
                fresh.clipCount = reused->clipCount;
                fresh.valid     = true;
                LOG_INFO("SceneInstanceLoader: '%s' reuses skeleton idx=%u (a registration reuse scope; registry "
                         "stays %u skeletons)", iscnPath.c_str(), fresh.skelIdx, renderer->GetSkeletonRegistry().Count());
            }
            else if (treg && d.valid && !d.meshBlends.empty())
            {
                // The template's registration: the .iskel was read for its blend
                // data only, because a part's shared source is gone (a world clear).
                fresh.skelIdx   = treg->skelIdx;
                fresh.clipCount = treg->clipCount;
                fresh.valid     = true;
                ++Resource::PrefabTemplate::Stats().skeletonReuses;
                LOG_INFO("SceneInstanceLoader: '%s' reuses skeleton idx=%u (the runtime spawn template; registry "
                         "stays %u skeletons)", iscnPath.c_str(), fresh.skelIdx, renderer->GetSkeletonRegistry().Count());
            }
            else if (d.valid && !d.meshBlends.empty())
            {
                // The virtual bones go after the real ones (never written to the .iskel).
                if (!modelVirtualBones.bones.empty())
                {
                    std::vector<std::string> warns;
                    const uint32_t added = Resource::ApplyVirtualBones(*d.skeleton, modelVirtualBones.bones, &warns);
                    for (const std::string& w : warns)
                        LOG_WARNING("SceneInstanceLoader: '%s' %s", iscnPath.c_str(), w.c_str());
                    LOG_INFO("SceneInstanceLoader: '%s' - %u virtual bone(s) after %u real bone(s)", iscnPath.c_str(),
                             added, d.skeleton->realBoneCount);
                }
                d.skeleton->virtualBoneSignature = vbSig;   // which VB records it has
                const auto _tG0 = _now();
                fresh.skelIdx = renderer->GetSkeletonRegistry().Register(std::move(*d.skeleton));
                for (auto& clip : d.clips)
                    renderer->GetClipLibrary().Register(std::move(clip));
                _msRegistry = std::chrono::duration<double, std::milli>(_now() - _tG0).count();

                fresh.clipCount = d.clips.size();
                fresh.valid     = true;
                if (!reuseKey.empty()) ReusedRegistrations()[reuseKey] = { fresh.skelIdx, fresh.clipCount };
                if (!tplKey.empty())
                {
                    TemplateRegistration& tr = TemplateRegistrations()[tplKey];
                    tr.skelIdx   = fresh.skelIdx;
                    tr.clipCount = fresh.clipCount;
                    tr.skinnedMeshes.clear();
                    for (const IskelMeshBlend& mb : d.meshBlends) tr.skinnedMeshes.push_back(mb.meshIndex);
                    ++Resource::PrefabTemplate::Stats().skeletonRegistrations;
                }

                LOG_INFO("SceneInstanceLoader: registered skeleton idx=%u, %zu clips "
                         "(registry now %u skeletons / %u clips, SkeletonAsset=%zu KB each)",
                         fresh.skelIdx, fresh.clipCount,
                         renderer->GetSkeletonRegistry().Count(),
                         renderer->GetClipLibrary().Count(),
                         sizeof(SkeletonAsset) / 1024);
            }
            fresh.meshBlends = std::move(d.meshBlends);

            if (g_batch)
                resolved = &g_batch->iskels.emplace(shareKey, std::move(fresh)).first->second;
            else
            {
                localShare = std::move(fresh);
                resolved   = &localShare;
            }
        }

        const IskelShare& iskel = *resolved;

        if (iskel.valid && !iskel.meshBlends.empty())
        {
            const uint32_t skelIdx = iskel.skelIdx;

            // Collapse this load's skinned-mesh buffer uploads into one
            // FlushUploadAndWait: RegisterSkinnedMeshFull opens its own nested upload
            // batch and the flat batch counter lets only the outermost scope flush, so
            // N characters x ~7 buffers cost one drain instead of one fence round trip
            // each. RAII closes the batch on the loop's early `continue` paths too.
            // Staging auto-flushes mid-loop at 64 MB / 512 buffers
            // (FlushBatchStagingIfNeeded), so big scenes do not run out of memory.
            // The buffer-HEAP batch suballocates the ~7 buffers per part (~210 for a
            // character) as placed resources from a few shared heaps instead of ~210
            // CreateCommittedResource calls (one implicit heap each), the dominant
            // main-thread commit cost.
            // The destructor is timed into _msUploadFlush: EndBufferUploadBatch
            // flushes when the depth reaches 0, once per Load() (per character
            // instance); it waits for the batch's copies on the COPY queue, and not at
            // all when every buffer came from a cache.
            struct LoopUploadBatch
            {
                IGraphicsDevice& g;
                double&          outMs;
                LoopUploadBatch(IGraphicsDevice& gg, double& ms) : g(gg), outMs(ms)
                { g.BeginBufferUploadBatch(); g.BeginBufferHeapBatch(); }
                ~LoopUploadBatch()
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    g.EndBufferHeapBatch(); g.EndBufferUploadBatch();
                    outMs += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0).count();
                }
                LoopUploadBatch(const LoopUploadBatch&) = delete;
                LoopUploadBatch& operator=(const LoopUploadBatch&) = delete;
            } _skinnedUploadBatch(gfx, _msUploadFlush);

            // Cache each unique .meshlib blob (a character's N parts share one
            // .meshlib; re-reading it per part costs ~330 ms) and each combined
            // .imorph (every submesh references the one file; blocks are indexed
            // by meshFileIndex).
            // These alias the BatchScope cache when one is open, extending the
            // dedupe across Load() calls (the repeated-character case, 100x Rossi
            // in AIBench); without a scope they are per-Load locals.
            std::unordered_map<std::string, std::vector<uint8_t>> localMeshlibBlobs;
            std::unordered_map<std::string, std::vector<uint8_t>> localImorphBlobs;
            auto& meshlibBlobCache = g_batch ? g_batch->meshlibBlobs : localMeshlibBlobs;
            auto& imorphBlobCache  = g_batch ? g_batch->imorphBlobs  : localImorphBlobs;


            // For each skinned mesh, build pending and register GPU buffers
            for (const IskelMeshBlend& mb : iskel.meshBlends)
            {
                const int fi = static_cast<int>(mb.meshIndex);
                auto it = meshFileToEntities.find(fi);
                if (it == meshFileToEntities.end() || it->second.empty())
                {
                    LOG_WARNING("SceneInstanceLoader: no entity for skinned meshFileIdx=%d", fi);
                    continue;
                }
                if (fi >= static_cast<int>(meshAbsPaths.size()) || meshAbsPaths[fi].empty())
                {
                    LOG_WARNING("SceneInstanceLoader: no mesh source path for meshFileIdx=%d", fi);
                    continue;
                }

                // The part's source is live in the renderer: no decode, no copies,
                // no morph block; the shared path takes the buffers, counts and morph
                // mapping from the source. Every part takes it when the template's
                // registration found them all live (nothing was read above);
                // otherwise, in a runtime spawn, each part whose own source is live
                // does (the first runtime spawn of a model a scene load already
                // placed reads the .iskel to register the template's skeleton, and
                // decodes nothing).
                if (sharedOnly ||
                    (tplActive && g_batch && renderer->HasSharedSkinnedSource(partKey(fi))))
                {
                    const uint64_t key = partKey(fi);
                    for (Entity meshEnt : it->second)
                    {
                        SceneLoader::SkinnedMeshPending pending;
                        pending.entity        = meshEnt;
                        pending.rootEntity    = rootEntity;
                        pending.skeletonIndex = skelIdx;
                        pending.sourceKey     = key;
                        pending.sharedOnly    = true;
                        LOG_INFO("SceneInstanceLoader:   meshFileIdx=%d entity=%u shared source (the runtime spawn "
                                 "template: not decoded)", fi, meshEnt);
                        const auto _tR0 = _now();
                        if (!renderer->RegisterSkinnedMeshFull(world, pending))
                            LOG_WARNING("SceneInstanceLoader: RegisterSkinnedMeshFull failed for entity=%u", meshEnt);
                        _msRegister += std::chrono::duration<double, std::milli>(_now() - _tR0).count();
                        ++Resource::PrefabTemplate::Stats().sharedParts;
                    }
                    continue;
                }

                // DecomposeMeshSource auto-detects .meshlib vs .imsh. For meshlib
                // it uses the F-line's meshId (fiMeshId[fi]) to slice out the
                // right entry's vertex/index range with local-rebased indices.
                // With a BatchScope open this is memoised across Load() calls,
                // so re-instantiating the same character skips the decode.
                const auto  _tD0 = _now();
                ImshStreams streamsScratch;
                const ImshStreams& streams = *DecodeMeshSourceCached(
                    meshAbsPaths[fi], fiMeshId[fi], streamsScratch, &meshlibBlobCache);
                _msDecompose += std::chrono::duration<double, std::milli>(_now() - _tD0).count();

                if (!streams.valid)
                {
                    LOG_WARNING("SceneInstanceLoader: failed to decompose mesh source for idx=%d path='%s'",
                                fi, meshAbsPaths[fi].c_str());
                    continue;
                }

                // Validate vertex count match between mesh source and .iskel blend data
                const uint32_t imshVerts  = static_cast<uint32_t>(streams.positions.size());
                const uint32_t blendVerts = static_cast<uint32_t>(mb.blendData.size());
                if (imshVerts != blendVerts)
                {
                    LOG_ERROR("SceneInstanceLoader: VERTEX COUNT MISMATCH! "
                              "meshFileIdx=%d mesh=%u src=%u verts .iskel blend=%u verts — skinning will be WRONG",
                              fi, fiMeshId[fi], imshVerts, blendVerts);
                }

                // Every entity spawned for this mesh file (typically one).
                for (Entity meshEnt : it->second)
                {
                    // Timed separately: seven full vector copies of the
                    // character's geometry into `pending` (positions, normals,
                    // uv, uv1, tangents, indices, blendData; ~6 MB for a
                    // 70k-vert character).
                    const auto _tP0 = _now();
                    SceneLoader::SkinnedMeshPending pending;
                    pending.entity        = meshEnt;
                    pending.rootEntity    = rootEntity;
                    pending.skeletonIndex = skelIdx;
                    pending.restPositions = streams.positions;
                    pending.restNormals   = streams.normals;
                    pending.uvs           = streams.uvs;
                    pending.uvs1          = streams.uvs1;
                    pending.uvs2          = streams.uvs2;
                    pending.uvs3          = streams.uvs3;
                    pending.tangents      = streams.tangents;
                    pending.indices       = streams.indices;
                    pending.blendData     = mb.blendData;

                    // Content identity of the streams above (partKey), so a
                    // second instance of this character borrows the first one's
                    // GPU buffers instead of uploading an identical copy.
                    //
                    // ONLY inside a BatchScope: outside one there is no cache
                    // to share through, and a key minted now would outlive the
                    // load and could collide with a re-imported asset.
                    if (g_batch)
                        pending.sourceKey = partKey(fi);
                    if (tplActive) ++Resource::PrefabTemplate::Stats().decodedParts;
                    _msPendingCopy += std::chrono::duration<double, std::milli>(_now() - _tP0).count();

                    // Load this part's .imorph block, if any. Resource::MorphLibrary
                    // reads every flavour (the single-block format, MORPHLIB
                    // dense/sparse, MORPHLIB2 section-keyed with normal deltas). The
                    // combined file is shared by every submesh, so the read is cached
                    // and the part's block found by its section key (v2) or its F-line
                    // index (v1).
                    // The extracted block is memoised per (imorphPath, meshFileIdx) for
                    // the BatchScope's life: the delta payload is
                    // morphCount * vertexCount * 12 B (twice with normal deltas), the
                    // largest single allocation of a character load.
                    const auto _tM0 = _now();
                    for (const auto& mref : morphFileRefs)
                    {
                        if (mref.meshFileIdx != fi) continue;

                        const std::string imorphPath = Sys::PathToUtf8(iscnDir / Sys::U8Path(mref.file));

                        if (g_batch)
                        {
                            auto mit = g_batch->morphs.find(CacheKey(imorphPath, uint32_t(fi)));
                            if (mit != g_batch->morphs.end())
                            {
                                ++g_batch->morphHit;
                                // The memo is keyed by (.imorph, F idx), not by the
                                // geometry, so re-check the one invariant the GPU
                                // relies on: deltas = morphCount x THIS mesh's verts
                                // (x2 with normal deltas).
                                const MorphBlock& mb = mit->second;
                                const bool fits = mb.deltas.size() ==
                                    static_cast<size_t>(mb.morphCount) * imshVerts * (mb.hasNormals ? 2u : 1u);
                                if (mb.found && !fits)
                                    LOG_ERROR("SceneInstanceLoader: cached .imorph block '%s' mesh=%d holds %zu "
                                              "deltas, the mesh needs %u x %u%s - morphs DISABLED for this part",
                                              mref.file.c_str(), fi, mb.deltas.size(), mb.morphCount, imshVerts,
                                              mb.hasNormals ? " x 2" : "");
                                if (mb.found && fits)
                                {
                                    pending.morphCount       = mb.morphCount;
                                    pending.morphDeltas      = mb.deltas;
                                    pending.morphHasNormals  = mb.hasNormals;
                                    pending.morphNames       = mb.names;
                                    pending.morphSlotOfLocal = mb.slotOfLocal;
                                }
                                break;
                            }
                            ++g_batch->morphMiss;
                        }

                        auto cit = imorphBlobCache.find(imorphPath);
                        if (cit == imorphBlobCache.end())
                        {
                            std::vector<uint8_t> mb;
                            ::Resource::AssetFS::Get().ReadFile(imorphPath, mb);  // read ONCE per unique .imorph
                            cit = imorphBlobCache.emplace(imorphPath, std::move(mb)).first;
                        }
                        const std::vector<uint8_t>& mblob = cit->second;

                        // Retained across the batch so the extracted block can be
                        // memoised below without re-reading `pending`.
                        MorphBlock _block;

                        Resource::MorphLibrary::File mfile;
                        std::string                  mwhy;
                        if (mblob.empty())
                        {
                            // No file: nothing to load (the read already said why).
                        }
                        else if (!Resource::MorphLibrary::Read(mblob.data(), mblob.size(), mfile, &mwhy))
                            LOG_ERROR("SceneInstanceLoader: .imorph '%s' does not read (%s) - morphs DISABLED "
                                      "for this part", mref.file.c_str(), mwhy.c_str());
                        else
                        {
                            // The part's section: the key of the library entry this
                            // F-line resolved to (the F-line's own `sec=` when the
                            // library has no identity).
                            uint64_t partKey = fileRecords[fi].sectionKey;
                            if (const auto id = meshLib.GetIdentity(fiLibHandle[fi]);
                                id && fiMeshId[fi] < id->sections.size())
                                partKey = id->sections[fiMeshId[fi]].key;
                            const Resource::MorphLibrary::Block* blk = mfile.Find(static_cast<uint32_t>(fi), partKey);
                            if (!blk)
                            {
                                const std::string sec = mfile.Keyed()
                                    ? " (section " + Resource::MeshKeyToHex(partKey) + ": written for other geometry?)"
                                    : std::string();
                                LOG_WARNING("SceneInstanceLoader: .imorph '%s' has no block for mesh=%d%s - "
                                            "morphs DISABLED for this part", mref.file.c_str(), fi, sec.c_str());
                            }
                            // A block is dense over the mesh's vertices (Skin.cs reads
                            // deltas[m * vertexCount + v]), so a vertex-count mismatch
                            // means this block was written for OTHER geometry - a stale
                            // .imorph next to a re-imported .meshlib. Uploading it
                            // would displace the wrong vertices with no error anywhere
                            // else.
                            else if (blk->vertexCount != imshVerts)
                                LOG_ERROR("SceneInstanceLoader: .imorph '%s' block for mesh=%d has %u verts but "
                                          "the mesh has %u - the morph file does not match this geometry (stale "
                                          "re-import?); morphs DISABLED for this part",
                                          mref.file.c_str(), fi, blk->vertexCount, imshVerts);
                            else
                            {
                                // ALWAYS produces a slot table (the identity for a
                                // dense block), and the GPU buffer's image: every
                                // target's positions, then every target's normals.
                                const size_t vc = blk->vertexCount;
                                const size_t n  = static_cast<size_t>(blk->count) * vc;
                                pending.morphCount      = blk->count;
                                pending.morphHasNormals = blk->hasNormals;
                                pending.morphDeltas.resize(blk->hasNormals ? 2 * n : n);
                                pending.morphNames.resize(blk->count);
                                pending.morphSlotOfLocal.resize(blk->count);
                                DirectX::XMFLOAT3* dst = pending.morphDeltas.data();
                                for (uint32_t t = 0; t < blk->count; ++t)
                                {
                                    pending.morphNames[t]       = blk->Name(t);
                                    pending.morphSlotOfLocal[t] = blk->Slot(t);
                                    blk->CopyPositions(t, dst + t * vc);
                                    if (blk->hasNormals) blk->CopyNormals(t, dst + n + t * vc);
                                }
                                _block.found      = true;
                                _block.morphCount = blk->count;
                                _block.hasNormals = blk->hasNormals;
                                LOG_INFO("SceneInstanceLoader:   .imorph(%s,%s%s) '%s' mesh=%d — %u morphs, %u verts",
                                         mfile.magic == Resource::MAGIC_MORPHLIB2 ? "v2"
                                         : mfile.magic == Resource::MAGIC_MORPHLIB ? "v1" : "legacy",
                                         blk->slots ? "sparse" : "dense", blk->hasNormals ? ",normals" : "",
                                         mref.file.c_str(), fi, blk->count, blk->vertexCount);
                            }
                        }

                        // Memoise the extracted block (including the "this file has
                        // no block for meshFileIdx" answer, which is what makes the
                        // entry scan above a one-off per character).
                        if (g_batch &&
                            g_batch->decodedBytes +
                                pending.morphDeltas.size() * sizeof(DirectX::XMFLOAT3) +
                                pending.morphSlotOfLocal.size() * sizeof(uint32_t)
                                <= kBatchCacheBudgetBytes)
                        {
                            if (_block.found)
                            {
                                _block.deltas      = pending.morphDeltas;
                                _block.names       = pending.morphNames;
                                _block.slotOfLocal = pending.morphSlotOfLocal;
                                g_batch->decodedBytes +=
                                    _block.deltas.size() * sizeof(DirectX::XMFLOAT3)
                                  + _block.slotOfLocal.size() * sizeof(uint32_t);
                            }
                            g_batch->morphs.emplace(CacheKey(imorphPath, uint32_t(fi)),
                                                    std::move(_block));
                        }
                        break;
                    }
                    _msMorph += std::chrono::duration<double, std::milli>(_now() - _tM0).count();

                    LOG_INFO("SceneInstanceLoader:   meshFileIdx=%d entity=%u verts=%u blend=%u morphs=%u",
                             fi, meshEnt, imshVerts, blendVerts, pending.morphCount);

                    const auto _tR0 = _now();
                    if (!renderer->RegisterSkinnedMeshFull(world, pending))
                        LOG_WARNING("SceneInstanceLoader: RegisterSkinnedMeshFull failed for entity=%u", meshEnt);
                    _msRegister += std::chrono::duration<double, std::milli>(_now() - _tR0).count();
                }
            }
        }
        else if (iskel.valid)
        {
            LOG_INFO("SceneInstanceLoader: .iskel has no blend data — static skeleton only");
        }
    }

    const auto _tSkinned = std::chrono::steady_clock::now();  // skinned registration + DecomposeMeshSource done
    stage.Next(s_stTail);
    LOG_INFO("[LOAD-PROFILE] SceneInstanceLoader '%s': read+parse %.1f | mesh-lib %.1f | "
             "spawn+material %.1f | skinned+decompose %.1f ms "
             "(of which iskel %.1f | registry %.1f | decode %.1f | morph %.1f | copy %.1f | register %.1f | uploadflush %.1f) (total %.1f) "
             "[parse+decompose+material = worker-threadable; spawn-ECS+upload = main-only]",
             iscnPath.c_str(),
             _ms(_t0, _tParse), _ms(_tParse, _tMeshLib), _ms(_tMeshLib, _tSpawn),
             _ms(_tSpawn, _tSkinned),
             _msIskel, _msRegistry, _msDecompose, _msMorph, _msPendingCopy, _msRegister, _msUploadFlush, _ms(_t0, _tSkinned));

    LOG_INFO("SceneInstanceLoader: material cache: %zu unique .imat loaded for %u mesh entities",
             matCache.size(), meshCount);

    // The model's sockets on this instance's skeleton root (attached above,
    // synchronously, by RegisterSkinnedMeshFull). -sockets_asset_off derives none,
    // so a follower of one finds no socket.
    if (rootEntity != NullEntity && renderer && modelSockets.SkeletonSocketCount() > 0 &&
        !Sys::CommandLine::Get().Has("-sockets_asset_off"))
    {
        const SkeletonRegistry& skeletons = renderer->GetSkeletonRegistry();
        if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(rootEntity);
            sk && sk->assetIndex != kInvalidSkeletonIndex && sk->assetIndex < skeletons.Count())
            DeriveModelSockets(world, rootEntity, skeletons.Get(sk->assetIndex), modelSockets, iscnPath);
        else
            LOG_WARNING("SceneInstanceLoader: '%s' has %zu socket record(s) but its root has no skeleton - none "
                        "published", iscnPath.c_str(), modelSockets.SkeletonSocketCount());
    }

    // Store .iscn path on root entity for prefab serialization
    if (rootEntity != NullEntity)
        world.AddComponent<SceneSourcePath>(rootEntity, SceneSourcePath{ iscnPath });

    result.rootEntity      = rootEntity;
    result.nodeCount       = nodeCount;
    result.meshEntityCount = meshCount;
    result.success         = (rootEntity != NullEntity);

    LOG_INFO("SceneInstanceLoader: done — %u node entities, %u mesh entities, "
             "%u colliders, root=%u",
             nodeCount, meshCount, colliderCount, rootEntity);
    if (assetCollision.collision)
    {
        LOG_INFO("SceneInstanceLoader: '%s' asset collision - %zu Mesh(es) with records, %u Mesh body(ies), "
                 "%u section trimesh(es)", iscnPath.c_str(), modelCollision.meshes.size(),
                 assetCollision.meshBodies, assetCollision.sectionBodies);
        if (assetCollision.conflicts > 0)
            LOG_WARNING("SceneInstanceLoader: '%s' - %u part(s) already had a collider (a C record) where the asset "
                        "collision would go; the C record's is kept", iscnPath.c_str(), assetCollision.conflicts);
    }

    return result;
}

// ==== .iscn material bindings: read + write-back ====
namespace
{
    // Shared by both entry points so the two can never disagree about which
    // lines are F-lines or how a `mat=` value maps to a project path.
    bool ReadIscnBlobForEdit(const std::string& iscnPath, std::vector<uint8_t>& blob,
                             std::string& text, const char* who)
    {
        if (!Sys::ReadFileBytes(iscnPath, blob) || blob.empty())
        {
            LOG_ERROR("%s: cannot read '%s'", who, iscnPath.c_str());
            return false;
        }
        if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
        {
            LOG_ERROR("%s: bad .iscn header in '%s'", who, iscnPath.c_str());
            return false;
        }
        const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        const void*  nul  = std::memchr(payl, 0, n);
        if (nul) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        text.assign(payl, n);
        return true;
    }

    // An F-line with its `mat=` replaced by @p matPath (project-relative; ""
    // drops the key). `mat=` is read to END OF LINE, so the rewrite is a plain
    // truncate-and-append: cut at " mat=" (or at the end when the line has
    // none) and write the new value last. Stored .iscn-DIRECTORY-relative,
    // which is what SpawnSingleNode resolves against; a material outside the
    // model's own folder yields a "../" chain - legal, and the only
    // expressible form.
    std::string FLineWithMaterial(const std::string& line, const std::string& matPath,
                                  const std::filesystem::path& iscnDir)
    {
        std::string head = line;
        const size_t mp  = line.find(" mat=");
        if (mp != std::string::npos) head = line.substr(0, mp);
        while (!head.empty() && head.back() == ' ') head.pop_back();

        std::string newVal;
        if (!matPath.empty())
        {
            std::error_code ec;
            const std::filesystem::path rel = std::filesystem::relative(
                Sys::U8Path(matPath).lexically_normal(), iscnDir.lexically_normal(), ec);
            newVal = (ec || rel.empty()) ? matPath : Sys::PathToUtf8(rel);
            for (char& c : newVal) if (c == '\\') c = '/';
        }
        return newVal.empty() ? head : head + " mat=" + newVal;
    }

    // Re-emit the container around @p text and write it atomically (temp +
    // rename). Normalised to the SceneImporter shape (one trailing NUL, dataSize =
    // textLength + 1) whatever shape the input had. Atomic because the Mesh Editor
    // saves a model other panels and the preview's watcher read: a reader must see
    // the old model or the new one, never a torn one.
    bool WriteIscnText(const std::string& iscnPath, const std::vector<uint8_t>& blob,
                       const std::string& text, const char* who)
    {
        const uint32_t textLen = static_cast<uint32_t>(text.size());

        Resource::SceneMetadata meta{};
        if (blob.size() >= sizeof(Resource::AssetHeader) + sizeof(Resource::SceneMetadata))
            std::memcpy(&meta, blob.data() + sizeof(Resource::AssetHeader),
                        sizeof(Resource::SceneMetadata));
        meta.textLength = textLen;

        Resource::AssetHeader hdr{};
        std::memcpy(&hdr, blob.data(), sizeof(Resource::AssetHeader));
        hdr.metadataSize = sizeof(Resource::SceneMetadata);
        hdr.dataSize     = textLen + 1;

        std::vector<uint8_t> outBlob(sizeof(Resource::AssetHeader)
                                     + sizeof(Resource::SceneMetadata) + textLen + 1, 0);
        uint8_t* dst = outBlob.data();
        std::memcpy(dst, &hdr,  sizeof(Resource::AssetHeader));    dst += sizeof(Resource::AssetHeader);
        std::memcpy(dst, &meta, sizeof(Resource::SceneMetadata));  dst += sizeof(Resource::SceneMetadata);
        std::memcpy(dst, text.data(), textLen);                     // trailing NUL already zeroed

        if (!Sys::WriteFileBytesAtomic(iscnPath, outBlob.data(), outBlob.size()))
        {
            LOG_ERROR("%s: cannot write '%s'", who, iscnPath.c_str());
            return false;
        }
        return true;
    }
}

std::vector<SceneInstanceLoader::FileReference>
SceneInstanceLoader::ListFileReferences(const std::string& text, const std::string& iscnPath)
{
    std::vector<FileReference> out;
    const std::filesystem::path iscnDir = Sys::U8Path(iscnPath).parent_path();
    auto resolve = [&](const std::string& rel)
    {
        std::string p = Sys::RelativizeAssetPath(Sys::PathToUtf8((iscnDir / Sys::U8Path(rel)).lexically_normal()));
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    };
    auto add = [&](char kind, const char* key, const std::string& rel, unsigned line)
    {
        if (!rel.empty()) out.push_back(FileReference{ kind, key, resolve(rel), line });
    };

    std::istringstream ss(text);
    std::string        line;
    unsigned           n = 0;
    while (std::getline(ss, line))
    {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A `PR` hull / mesh prim naming a collision library (`lib=`, one
        // percent-encoded token; absent = the Mesh's own library).
        if (line.size() > 3 && line[0] == 'P' && line[1] == 'R' && line[2] == ' ')
        {
            const size_t at = line.find(" lib=");
            if (at == std::string::npos) continue;
            const size_t s = at + 5, e = line.find(' ', s);
            add('P', "lib", Resource::AssetRefs::PercentDecode(line.substr(s, e == std::string::npos ? std::string::npos : e - s)), n);
            continue;
        }
        if (line.size() < 3 || line[1] != ' ') continue;
        if (line[0] == 'F')
        {
            FileRecord fr;
            if (!ParseFLine(line, fr)) continue;
            add('F', "file", fr.file, n);
            add('F', "mat", fr.matFile, n);
        }
        else if (line[0] == 'C' || line[0] == 'V')
        {
            // A collision record's own library (hulls: <Scene>_collision.meshlib).
            CollisionRecord cr;
            if (ParseCLine(line, cr)) add(line[0], "lib", cr.lib, n);
        }
        else if (line[0] == 'K')
        {
            std::string file;
            if (ParseKLine(line, file)) add('K', "file", file, n);
        }
        else if (line[0] == 'M')
        {
            // Load()'s M-line read: file= runs to the end of the line.
            const size_t filePos = line.find("file=");
            if (filePos == std::string::npos) continue;
            std::string file = line.substr(filePos + 5);
            while (!file.empty() && (file.back() == ' ' || file.back() == '\r' || file.back() == '\n'))
                file.pop_back();
            add('M', "file", file, n);
        }
    }
    return out;
}

bool SceneInstanceLoader::ReadMaterialBindings(const std::string&            iscnPath,
                                              std::vector<MaterialBinding>& out)
{
    out.clear();

    // AssetFS, not Sys: reading works on a packed build too.
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob) || blob.empty())
    {
        LOG_ERROR("SceneInstanceLoader::ReadMaterialBindings: cannot open '%s'", iscnPath.c_str());
        return false;
    }
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
    {
        LOG_ERROR("SceneInstanceLoader::ReadMaterialBindings: bad header in '%s'", iscnPath.c_str());
        return false;
    }

    std::string text;
    {
        const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        const void*  nul  = std::memchr(payl, 0, n);
        if (nul) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        text.assign(payl, n);
    }

    const std::filesystem::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    // Keyed by (mesh file, meshId), never by meshId alone: `mesh=` is unique only
    // within one mesh file, and an .iscn with one .imsh per F-line has no `mesh=` at
    // all (all 70 rows of knight.iscn parse to meshId 0). This is also the composite
    // key SaveScene looks rows up by (MeshSourcePath + '#' + meshId).
    std::unordered_map<std::string, size_t> byMesh;
    std::istringstream ss(text);
    std::string        line;
    while (std::getline(ss, line))
    {
        if (line.empty() || line[0] == '#') continue;
        if (!(line.size() > 2 && line[0] == 'F' && line[1] == ' ')) continue;

        FileRecord fr;
        if (!ParseFLine(line, fr)) continue;

        // Exactly the resolution SpawnSingleNode performs, so a caller can
        // byte-compare this against a live MaterialSourcePath.
        std::string matPath;
        if (!fr.matFile.empty())
            matPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                (iscnDir / Sys::U8Path(fr.matFile)).lexically_normal()));

        // VERBATIM the expression at the meshAbsPaths[] fill site, so the string
        // compares equal to a live MeshSourcePath byte for byte.
        const std::string meshAbs = Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file));
        const std::string key     = meshAbs + '#' + std::to_string(fr.subIdx);

        auto it = byMesh.find(key);
        if (it == byMesh.end())
        {
            byMesh.emplace(key, out.size());
            MaterialBinding mb;
            mb.meshAbsPath = meshAbs;
            mb.meshId      = fr.subIdx;
            mb.sectionKey  = fr.sectionKey;
            mb.matPath     = std::move(matPath);
            mb.name        = fr.name;
            out.push_back(std::move(mb));
        }
        else if (out[it->second].matPath != matPath)
        {
            // Unreal level imports point several F-lines at one meshlib entry
            // with different materials on purpose (the "entry|mat" dedup key).
            // meshId alone cannot address those, so flag rather than guess.
            out[it->second].ambiguous = true;
        }
    }
    return true;
}

bool SceneInstanceLoader::WriteMaterialBindings(const std::string&                  iscnPath,
                                                const std::vector<MaterialBinding>& bindings,
                                                int*                                outChanged)
{
    if (outChanged) *outChanged = 0;
    if (bindings.empty()) return true;

    std::vector<uint8_t> blob;
    std::string          text;
    if (!ReadIscnBlobForEdit(iscnPath, blob, text,
                             "SceneInstanceLoader::WriteMaterialBindings"))
        return false;

    // Which keys are addressable at all, recomputed from the file rather than
    // trusted from the caller (whose view came from the world; the file is what gets
    // overwritten). Counted per (mesh file, meshId), never per meshId alone: in an
    // .iscn with one .imsh per F-line every row is meshId 0, and an id-keyed count
    // would call all of them one ambiguous entry and refuse every write.
    const std::filesystem::path probeDir = Sys::U8Path(iscnPath).parent_path();
    auto lineKey = [&](const FileRecord& fr)
    {
        return Sys::PathToUtf8(probeDir / Sys::U8Path(fr.file)) + '#' + std::to_string(fr.subIdx);
    };
    std::unordered_map<std::string, int> fLinesPerMesh;
    {
        std::istringstream probe(text);
        std::string        pl;
        while (std::getline(probe, pl))
        {
            if (!(pl.size() > 2 && pl[0] == 'F' && pl[1] == ' ')) continue;
            FileRecord fr;
            if (ParseFLine(pl, fr)) ++fLinesPerMesh[lineKey(fr)];
        }
    }

    // Keyed the same way, so a caller that passes meshAbsPath addresses exactly
    // one F-line even in a multi-file .iscn. A caller that leaves meshAbsPath empty
    // (the common single-.meshlib case) is matched by meshId alone, unambiguous
    // there by construction.
    std::unordered_map<std::string, std::string> want;
    auto resolveKey = [&](const MaterialBinding& b) -> std::string
    {
        if (!b.meshAbsPath.empty())
            return b.meshAbsPath + '#' + std::to_string(b.meshId);
        std::string only;
        int hits = 0;
        const std::string suffix = '#' + std::to_string(b.meshId);
        for (const auto& [k, n] : fLinesPerMesh)
            if (k.size() > suffix.size() &&
                k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
            { only = k; hits += n; }
        return (hits == 1) ? only : std::string();
    };
    for (const MaterialBinding& b : bindings)
    {
        const std::string bk = resolveKey(b);
        const auto it = bk.empty() ? fLinesPerMesh.end() : fLinesPerMesh.find(bk);
        if (it == fLinesPerMesh.end())
        {
            LOG_WARNING("SceneInstanceLoader::WriteMaterialBindings: '%s' has no F-line for "
                        "mesh=%u — skipped", iscnPath.c_str(), b.meshId);
            continue;
        }
        if (it->second > 1)
        {
            LOG_WARNING("SceneInstanceLoader::WriteMaterialBindings: mesh=%u is named by %d "
                        "F-lines in '%s' with different materials — ambiguous, skipped",
                        b.meshId, it->second, iscnPath.c_str());
            continue;
        }
        want[bk] = b.matPath;
    }
    if (want.empty()) return true;

    const std::filesystem::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    // Rebuild the text line by line. Everything that is not a rewritten F-line
    // is copied through unchanged, including comments and unknown records.
    std::string        rebuilt;
    rebuilt.reserve(text.size() + want.size() * 64);
    std::istringstream ss(text);
    std::string        line;
    int                changed = 0;

    while (std::getline(ss, line))
    {
        // getline leaves the CR of a CRLF file on the line; keep it so the
        // file's own convention survives the round trip.
        std::string eol = "\n";
        if (!line.empty() && line.back() == '\r') { line.pop_back(); eol = "\r\n"; }

        bool rewritten = false;
        if (line.size() > 2 && line[0] == 'F' && line[1] == ' ')
        {
            FileRecord fr;
            if (ParseFLine(line, fr))
            {
                const auto it = want.find(lineKey(fr));
                if (it != want.end())
                {
                    const std::string out = FLineWithMaterial(line, it->second, iscnDir);
                    if (out != line) ++changed;
                    rebuilt += out;
                    rebuilt += eol;
                    rewritten = true;
                }
            }
        }
        if (!rewritten)
        {
            rebuilt += line;
            rebuilt += eol;
        }
    }

    if (changed == 0)
    {
        if (outChanged) *outChanged = 0;
        return true;   // nothing to do; leave the file's mtime alone
    }

    if (!WriteIscnText(iscnPath, blob, rebuilt, "SceneInstanceLoader::WriteMaterialBindings"))
        return false;

    if (outChanged) *outChanged = changed;
    LOG_SUCCESS("SceneInstanceLoader::WriteMaterialBindings: %d F-line(s) repointed in '%s'",
                changed, iscnPath.c_str());
    return true;
}

// The Mesh Editor's Save (see the header).
// The default is the FIRST F-line (MeshAssetRegistry::BuildModel's rule): every
// later line naming the same entry with the same material is the default placed
// again (four wheels of one car) and moves with it, while a line with another
// material is a placement override and keeps it. WriteMaterialBindings refuses
// both shapes ("ambiguous") because its caller cannot tell them apart; this one
// can.
bool SceneInstanceLoader::WriteDefaultMaterials(const std::string&                     iscnPath,
                                                const std::vector<DefaultMaterialEdit>& edits,
                                                int* outChanged, std::string* outError)
{
    if (outChanged) *outChanged = 0;
    auto fail = [&](std::string why)
    {
        if (outError) *outError = why;
        LOG_ERROR("SceneInstanceLoader::WriteDefaultMaterials: %s", why.c_str());
        return false;
    };
    if (edits.empty()) return true;

    std::vector<uint8_t> blob;
    std::string          text;
    if (!ReadIscnBlobForEdit(iscnPath, blob, text, "SceneInstanceLoader::WriteDefaultMaterials"))
        return fail("cannot read '" + iscnPath + "' as a model");

    const std::filesystem::path iscnDir = Sys::U8Path(iscnPath).parent_path();
    // The same two expressions ReadMaterialBindings uses, so a caller's
    // MaterialBinding::meshAbsPath / matPath compare equal byte for byte.
    auto lineKey = [&](const FileRecord& fr)
    {
        return Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file)) + '#' + std::to_string(fr.subIdx);
    };
    auto lineMat = [&](const FileRecord& fr) -> std::string
    {
        if (fr.matFile.empty()) return {};
        return Sys::RelativizeAssetPath(Sys::PathToUtf8((iscnDir / Sys::U8Path(fr.matFile)).lexically_normal()));
    };

    // Which edit an F-line answers to. A keyed line (`sec=`) answers to the edit
    // with its key (a stale `mesh=` hint cannot misdirect it) or else to an unkeyed
    // edit its hint names; an unkeyed line answers to the edit its hint names.
    auto keyOfKey = [](const std::string& abs, uint64_t k)
    {
        return abs + "#k" + Resource::MeshKeyToHex(k);
    };
    std::vector<std::string> newMat(edits.size());
    std::unordered_map<std::string, size_t> byKey, byHint;   // form -> edit index
    for (size_t i = 0; i < edits.size(); ++i)
    {
        const DefaultMaterialEdit& e = edits[i];
        newMat[i] = Sys::RelativizeAssetPath(e.material);
        byHint[e.meshAbsPath + '#' + std::to_string(e.meshId)] = i;
        if (e.sectionKey) byKey[keyOfKey(e.meshAbsPath, e.sectionKey)] = i;
    }
    auto matchOf = [&](const FileRecord& fr) -> int
    {
        const auto h = byHint.find(lineKey(fr));
        if (fr.sectionKey)
        {
            const std::string abs = Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file));
            if (const auto k = byKey.find(keyOfKey(abs, fr.sectionKey)); k != byKey.end())
                return static_cast<int>(k->second);
            return (h != byHint.end() && edits[h->second].sectionKey == 0) ? static_cast<int>(h->second) : -1;
        }
        return h != byHint.end() ? static_cast<int>(h->second) : -1;
    };

    // Pass 1: today's default of every edited entry.
    std::vector<int>         seen(edits.size(), 0);
    std::vector<std::string> firstMat(edits.size());
    {
        std::istringstream probe(text);
        std::string        pl;
        while (std::getline(probe, pl))
        {
            if (!pl.empty() && pl.back() == '\r') pl.pop_back();
            if (!(pl.size() > 2 && pl[0] == 'F' && pl[1] == ' ')) continue;
            FileRecord fr;
            if (!ParseFLine(pl, fr)) continue;
            const int i = matchOf(fr);
            if (i >= 0 && !seen[static_cast<size_t>(i)])
            {
                seen[static_cast<size_t>(i)]     = 1;
                firstMat[static_cast<size_t>(i)] = lineMat(fr);
            }
        }
    }
    // ALL OR NOTHING: an entry this model does not place takes its default
    // from the library's own model file, and writing the rest would save a
    // model the caller did not ask for.
    for (size_t i = 0; i < edits.size(); ++i)
        if (!seen[i])
            return fail("'" + iscnPath + "' has no F-line for " + edits[i].meshAbsPath + " entry " +
                        std::to_string(edits[i].meshId) +
                        (edits[i].sectionKey ? " (section " + Resource::MeshKeyToHex(edits[i].sectionKey) + ")"
                                             : std::string()) +
                        " - its default material comes from the library's own model");

    // Pass 2: rebuild, the file's line endings kept.
    std::string        rebuilt;
    rebuilt.reserve(text.size() + edits.size() * 64);
    std::istringstream ss(text);
    std::string        line;
    int                changed = 0;
    while (std::getline(ss, line))
    {
        std::string eol = "\n";
        if (!line.empty() && line.back() == '\r') { line.pop_back(); eol = "\r\n"; }
        if (line.size() > 2 && line[0] == 'F' && line[1] == ' ')
        {
            FileRecord fr;
            if (ParseFLine(line, fr))
            {
                const int i = matchOf(fr);
                if (i >= 0 && lineMat(fr) == firstMat[static_cast<size_t>(i)])
                {
                    const std::string out = FLineWithMaterial(line, newMat[static_cast<size_t>(i)], iscnDir);
                    if (out != line) ++changed;
                    rebuilt += out;
                    rebuilt += eol;
                    continue;
                }
            }
        }
        rebuilt += line;
        rebuilt += eol;
    }
    if (changed == 0) return true;   // nothing to do; leave the file's mtime alone
    if (!WriteIscnText(iscnPath, blob, rebuilt, "SceneInstanceLoader::WriteDefaultMaterials"))
        return fail("cannot write '" + iscnPath + "'");
    if (outChanged) *outChanged = changed;
    LOG_INFO("SceneInstanceLoader::WriteDefaultMaterials: %d F-line(s) repointed in '%s' (%zu slot(s))",
             changed, iscnPath.c_str(), edits.size());
    return true;
}

// ==== Keys in the model file (StampModelKeys; see the header) ====
const Resource::MeshIdentity*
SceneInstanceLoader::LibraryIdentityCache::Get(const std::string& meshlibPath)
{
    const std::string k = Resource::MeshLibCanonicalPath(meshlibPath);
    if (auto it = m_ids.find(k); it != m_ids.end()) return it->second.get();
    std::unique_ptr<Resource::MeshIdentity> id;
    std::vector<uint8_t> blob;
    if (::Resource::AssetFS::Get().ReadFile(meshlibPath, blob) &&
        Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
    {
        id = std::make_unique<Resource::MeshIdentity>();
        std::string why;
        // The file's own identity, else the synthesis MeshLibrary's provider
        // would make (the library's same-stem model groups it).
        if (Resource::ParseMeshLibIdentity(blob.data(), blob.size(), *id, &why) != Resource::MeshIdentityParse::Ok)
            *id = SynthesizeMeshLibIdentity(meshlibPath, blob.data(), blob.size());
    }
    const Resource::MeshIdentity* raw = id.get();
    m_ids.emplace(k, std::move(id));
    return raw;
}

namespace
{
    bool IsMeshlibFile(const std::string& f)
    {
        if (f.size() < 8) return false;
        std::string e = f.substr(f.size() - 8);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".meshlib";
    }

    // `<key>=<value>` token of @p line starting at the " <key>=" occurrence @p at:
    // [value start, value end) - the value runs to the next space (or the end).
    std::pair<size_t, size_t> TokenValue(const std::string& line, size_t at, size_t keyLen)
    {
        const size_t s = at + keyLen;
        const size_t e = line.find(' ', s);
        return { s, e == std::string::npos ? line.size() : e };
    }
}

bool SceneInstanceLoader::StampModelKeys(const std::string& iscnPath, bool apply, ModelKeyReport& rep,
                                         LibraryIdentityCache& libs, std::string* outError)
{
    rep = ModelKeyReport{};
    auto fail = [&](std::string why)
    {
        if (outError) *outError = why;
        return false;
    };
    std::vector<uint8_t> blob;
    std::string          text;
    if (!ReadIscnBlobForEdit(iscnPath, blob, text, "SceneInstanceLoader::StampModelKeys"))
        return fail("cannot read '" + iscnPath + "' as a model");
    const std::filesystem::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    // The file as lines, each with its own line ending (a CRLF file stays one).
    struct Line { std::string text; std::string eol; };
    std::vector<Line> lines;
    {
        size_t b = 0;
        while (b < text.size())
        {
            const size_t e = text.find('\n', b);
            Line l;
            l.text = text.substr(b, e == std::string::npos ? std::string::npos : e - b);
            l.eol  = (e == std::string::npos) ? "" : "\n";
            if (!l.text.empty() && l.text.back() == '\r') { l.text.pop_back(); l.eol = "\r" + l.eol; }
            lines.push_back(std::move(l));
            if (e == std::string::npos) break;
            b = e + 1;
        }
    }
    const std::string eol = [&]
    {
        for (const Line& l : lines) if (!l.eol.empty()) return l.eol;
        return std::string("\n");
    }();

    // ---- F-lines, in file order (their position is what N-lines' mr<k> name) ----
    struct FInfo { const Resource::MeshIdentity* id = nullptr; int entry = -1; uint64_t key = 0; bool legacy = false; };
    std::vector<FInfo> fs;
    for (Line& l : lines)
    {
        if (!(l.text.size() > 2 && l.text[0] == 'F' && l.text[1] == ' ')) continue;
        FileRecord fr;
        if (!ParseFLine(l.text, fr)) { fs.emplace_back(); continue; }   // the loader drops it too
        FInfo info;
        if (!IsMeshlibFile(fr.file))   // a per-mesh file (.imsh): no identity to key it by
        {
            info.legacy = true;
            ++rep.fLegacy;
            fs.push_back(info);
            continue;
        }
        ++rep.fLines;
        const size_t meshAt = l.text.find(" mesh=");
        if (meshAt == std::string::npos) { ++rep.fUnresolved; fs.push_back(info); continue; }
        const std::string lib = Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file));   // the loader's spelling
        info.id = libs.Get(lib);
        if (!info.id) { ++rep.fUnresolved; fs.push_back(info); continue; }

        if (fr.sectionKey != 0)
        {
            info.key = fr.sectionKey;
            const int byKey = info.id->FindSection(fr.sectionKey);
            if (byKey < 0) { ++rep.fDangling; fs.push_back(info); continue; }   // left alone
            info.entry = byKey;
            if (static_cast<uint32_t>(byKey) == fr.subIdx) ++rep.fKept;
            else
            {
                // The `mesh=` hint (what a binary without key support reads),
                // refreshed from the key.
                const auto [s, e] = TokenValue(l.text, meshAt, 6);
                l.text.replace(s, e - s, std::to_string(byKey));
                ++rep.fHintFixed;
            }
        }
        else if (fr.subIdx < info.id->sections.size())
        {
            info.entry = static_cast<int>(fr.subIdx);
            info.key   = info.id->sections[fr.subIdx].key;
            // `sec=` right after the mesh= token: before name= / mat= (mat= runs
            // to the end of the line).
            const auto [s, e] = TokenValue(l.text, meshAt, 6);
            l.text.insert(e, " sec=" + Resource::MeshKeyToHex(info.key));
            ++rep.fAdded;
        }
        else { ++rep.fUnresolved; info.id = nullptr; }
        fs.push_back(info);
    }

    // ---- N-lines: the one Mesh their refs are sections of ----
    std::vector<std::pair<uint64_t, std::string>> placed;   // ME: key, name - first placement order
    for (Line& l : lines)
    {
        if (!(l.text.size() > 2 && l.text[0] == 'N' && l.text[1] == ' ')) continue;
        NodeRecord nr;
        if (!ParseNLine(l.text, nr) || nr.meshFileIndices.empty()) continue;
        ++rep.nLines;
        if (std::all_of(nr.meshFileIndices.begin(), nr.meshFileIndices.end(), [&](int fi)
                        { return fi >= 0 && static_cast<size_t>(fi) < fs.size() && fs[static_cast<size_t>(fi)].legacy; }))
        {
            ++rep.nLegacy;
            continue;
        }
        const Resource::MeshIdentity* id = nullptr;
        uint32_t owner = UINT32_MAX;
        bool     one   = true;
        for (int fi : nr.meshFileIndices)
        {
            if (fi < 0 || static_cast<size_t>(fi) >= fs.size() || !fs[fi].id || fs[fi].entry < 0) { one = false; break; }
            const FInfo& f = fs[static_cast<size_t>(fi)];
            const uint32_t o = f.id->sections[static_cast<size_t>(f.entry)].ownerMesh;
            if (o == UINT32_MAX || (id && (f.id != id || o != owner))) { one = false; break; }
            id    = f.id;
            owner = o;
        }
        if (!one || !id || owner >= id->meshes.size()) { ++rep.nMixed; continue; }
        const Resource::MeshIdentity::Mesh& m = id->meshes[owner];
        if (std::find_if(placed.begin(), placed.end(), [&](const auto& p) { return p.first == m.key; }) == placed.end())
            placed.emplace_back(m.key, m.name);
        const std::string hex = Resource::MeshKeyToHex(m.key);
        // name= runs to the end of the line, so mkey= goes before it.
        const size_t nameAt = l.text.find(" name=");
        const size_t keyAt  = l.text.find(" mkey=");
        if (keyAt != std::string::npos && (nameAt == std::string::npos || keyAt < nameAt))
        {
            const auto [s, e] = TokenValue(l.text, keyAt, 6);
            if (l.text.compare(s, e - s, hex) == 0) ++rep.nKept;
            else { l.text.replace(s, e - s, hex); ++rep.nChanged; }
        }
        else
        {
            l.text.insert(nameAt == std::string::npos ? l.text.size() : nameAt, " mkey=" + hex);
            ++rep.nAdded;
        }
    }

    // ---- ME records, regenerated at the end of the file ----
    std::string rebuilt;
    rebuilt.reserve(text.size() + 32 * (rep.fLines + rep.nLines) + 64 * placed.size());
    for (const Line& l : lines)
    {
        if (l.text.size() > 3 && l.text[0] == 'M' && l.text[1] == 'E' && l.text[2] == ' ') continue;
        rebuilt += l.text;
        rebuilt += l.eol;
    }
    if (!placed.empty() && !rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
    for (const auto& [key, name] : placed)
    {
        rebuilt += "ME key=" + Resource::MeshKeyToHex(key) + " name=" +
                   Resource::AssetRefs::PercentEncodeStrict(name) + eol;
        ++rep.meLines;
    }

    rep.changed = (rebuilt != text);
    if (!apply || !rep.changed) return true;
    if (!WriteIscnText(iscnPath, blob, rebuilt, "SceneInstanceLoader::StampModelKeys"))
        return fail("cannot write '" + iscnPath + "'");
    rep.written = true;
    return true;
}

// ==== C records -> BO / PR (MigrateModelCollision; see the header) ====
namespace
{
    // The F-lines of a model as the loader sees them: only the lines ParseFLine
    // accepts count, in file order (their POSITION is what `mr<k>` and a C
    // record's `f=` name), each resolved to its library's identity key first.
    struct ModelFLine
    {
        FileRecord                    rec;
        std::string                   lib;          // the loader's meshAbsPath spelling
        const Resource::MeshIdentity* id = nullptr;
        int                           entry   = -1;
        uint64_t                      secKey  = 0;
        uint64_t                      meshKey = 0;  // the Mesh owning the section (0: unknown)
    };

    // The model's N / F / C lines, parsed exactly as Load() parses them.
    struct ModelRecords
    {
        std::vector<NodeRecord>                                nodes;
        std::vector<FileRecord>                                files;
        std::unordered_map<int, std::vector<CollisionRecord>>  collision;   // by F position
        std::unordered_map<int, std::vector<size_t>>           collisionLine;   // by F position: line numbers (0-based)
    };

    std::vector<ModelFLine> ResolveModelFLines(const std::vector<FileRecord>& files, const fs::path& iscnDir,
                                               SceneInstanceLoader::LibraryIdentityCache& libs)
    {
        std::vector<ModelFLine> out;
        out.reserve(files.size());
        for (const FileRecord& fr : files)
        {
            ModelFLine f;
            f.rec = fr;
            f.lib = Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file));
            if (IsMeshlibFile(fr.file)) f.id = libs.Get(f.lib);
            if (f.id)
            {
                f.entry = static_cast<int>(fr.subIdx);
                if (fr.sectionKey != 0)
                    if (const int byKey = f.id->FindSection(fr.sectionKey); byKey >= 0) f.entry = byKey;
                if (f.entry >= 0 && static_cast<size_t>(f.entry) < f.id->sections.size())
                {
                    const Resource::MeshIdentity::Section& s = f.id->sections[static_cast<size_t>(f.entry)];
                    f.secKey = s.key;
                    if (s.ownerMesh < f.id->meshes.size()) f.meshKey = f.id->meshes[s.ownerMesh].key;
                }
                else f.entry = -1;
            }
            out.push_back(std::move(f));
        }
        return out;
    }

    void ParseModelRecords(const std::vector<std::string>& lines, ModelRecords& out)
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string& line = lines[i];
            if (line.size() < 2 || line[0] == '#' || line[1] != ' ') continue;
            if (line[0] == 'N')
            {
                NodeRecord nr;
                if (ParseNLine(line, nr)) out.nodes.push_back(std::move(nr));
            }
            else if (line[0] == 'F')
            {
                FileRecord fr;
                if (ParseFLine(line, fr)) out.files.push_back(std::move(fr));
            }
            else if (line[0] == 'C')
            {
                CollisionRecord cr;
                if (ParseCLine(line, cr) && cr.fileIdx >= 0)
                {
                    out.collision[cr.fileIdx].push_back(std::move(cr));
                    out.collisionLine[cr.fileIdx].push_back(i);
                }
            }
        }
    }

    bool NonZero3(const DirectX::XMFLOAT3& v)
    {
        return std::fabs(v.x) > 1e-7f || std::fabs(v.y) > 1e-7f || std::fabs(v.z) > 1e-7f;
    }

    // One C record as the Mesh-level prim it becomes. `ownTrimesh` = a trimesh of
    // the part's own section (a `PR type=mesh sec=` without lib=, not a body prim).
    bool PrimFromCRecord(const CollisionRecord& cr, const ModelFLine& f, const fs::path& iscnDir,
                         Resource::CollisionPrim& out, bool& ownTrimesh, std::string& why)
    {
        using Prim = Resource::CollisionPrim;
        out = Prim{};
        ownTrimesh = false;
        // The C path composes a rotation INSIDE the entity scale and a compound
        // child's outside it: they differ under a non-uniform scale. No shipped C
        // record is rotated; one that is keeps its C record.
        if (NonZero3(cr.rotEuler)) { why = "a rotated primitive"; return false; }
        out.center = cr.offset;
        const auto libKey = [&](const std::string& lib, uint32_t entry) -> uint64_t
        {
            const std::string resolved = Sys::RelativizeAssetPath(Sys::PathToUtf8((iscnDir / Sys::U8Path(lib)).lexically_normal()));
            const auto table = Resource::MeshLibKeyTableAt(resolved);
            return table ? table->KeyOf(entry) : 0;
        };
        switch (static_cast<ColliderComponent::Shape>(cr.shape))
        {
        case ColliderComponent::Shape::Box:
            out.type = Prim::Type::Box;
            out.halfExtents = cr.halfExtents;
            return true;
        case ColliderComponent::Shape::Sphere:
            out.type = Prim::Type::Sphere;
            out.radius = cr.radius;
            return true;
        case ColliderComponent::Shape::Capsule:
            out.type = Prim::Type::Capsule;
            out.radius = cr.radius;
            out.halfHeight = cr.halfHeight;
            return true;
        case ColliderComponent::Shape::Mesh:
        case ColliderComponent::Shape::ConvexHull:
        {
            const bool hull = static_cast<ColliderComponent::Shape>(cr.shape) == ColliderComponent::Shape::ConvexHull;
            out.type = hull ? Prim::Type::Hull : Prim::Type::Mesh;
            if (cr.lib.empty())
            {
                if (f.secKey == 0) { why = "the part's section has no key"; return false; }
                if (!hull)
                {
                    if (NonZero3(cr.offset)) { why = "an offset trimesh of the part's own section"; return false; }
                    ownTrimesh = true;
                    out.sectionKey = f.secKey;
                    return true;
                }
                out.lib = f.rec.file;          // a hull of the part's own section, named by its library
                out.sectionKey = f.secKey;
                return true;
            }
            out.lib = cr.lib;
            out.sectionKey = libKey(cr.lib, cr.meshId);
            if (out.sectionKey == 0) { why = "'" + cr.lib + "' does not read (or has no entry " + std::to_string(cr.meshId) + ")"; return false; }
            return true;
        }
        default:
            why = "shape " + std::to_string(cr.shape) + " is not a collision primitive";
            return false;
        }
    }

    // A prim's exact text (the signature placements are compared by).
    std::string PrimSignature(const Resource::CollisionPrim& p)
    {
        Resource::MeshBodySetup one;
        one.prims.push_back(p);
        const std::string t = Resource::FormatBodySetup(one);
        const size_t nl = t.find('\n');
        return nl == std::string::npos ? std::string() : t.substr(nl + 1);   // the PR line (BO dropped)
    }
}

bool SceneInstanceLoader::MigrateModelCollision(const std::string& iscnPath, bool apply, CollisionMigrationReport& rep,
                                                LibraryIdentityCache& libs, std::string* outError)
{
    rep = CollisionMigrationReport{};
    auto fail = [&](std::string why)
    {
        if (outError) *outError = why;
        return false;
    };
    std::vector<uint8_t> blob;
    std::string          text;
    if (!ReadIscnBlobForEdit(iscnPath, blob, text, "SceneInstanceLoader::MigrateModelCollision"))
        return fail("cannot read '" + iscnPath + "' as a model");
    const fs::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    // Lines with their own endings (a CRLF file stays one).
    std::vector<std::string> lines, eols;
    for (size_t b = 0; b < text.size();)
    {
        const size_t e = text.find('\n', b);
        std::string l = text.substr(b, e == std::string::npos ? std::string::npos : e - b);
        std::string eol = (e == std::string::npos) ? "" : "\n";
        if (!l.empty() && l.back() == '\r') { l.pop_back(); eol = "\r" + eol; }
        lines.push_back(std::move(l));
        eols.push_back(std::move(eol));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    const std::string eol = [&]
    {
        for (const std::string& x : eols) if (!x.empty()) return x;
        return std::string("\n");
    }();

    ModelRecords recs;
    ParseModelRecords(lines, recs);
    for (const auto& [fi, v] : recs.collision) rep.cLines += static_cast<uint32_t>(v.size());
    Resource::ModelCollision existing;
    Resource::ParseModelCollision(text, existing);
    for (const std::string& l : lines)
        if (l.size() > 3 && l[0] == 'B' && l[1] == 'O' && l[2] == ' ') ++rep.boLines;
    if (rep.cLines == 0) return true;

    const std::vector<ModelFLine> fl = ResolveModelFLines(recs.files, iscnDir, libs);

    // ---- Per Mesh: what every placement of it carries ----
    struct MeshPlan
    {
        std::vector<std::string>            bodySig;          // the first placement's body prims
        std::vector<Resource::CollisionPrim> body;
        bool                                seen = false;
        std::unordered_map<uint64_t, bool>  trimesh;          // section key -> has one
        std::vector<uint64_t>               trimeshOrder;     // first-seen order
        int                                 layer = -1;       // -1 unset
        int                                 trigger = -1;
        uint32_t                            placements = 0;
        std::string                         refuse;
        const Resource::MeshIdentity*       id = nullptr;
        uint32_t                            owner = UINT32_MAX;
    };
    std::unordered_map<uint64_t, MeshPlan> plans;
    std::vector<uint64_t>                  planOrder;
    auto planOf = [&](uint64_t mk) -> MeshPlan&
    {
        auto [it, fresh] = plans.try_emplace(mk);
        if (fresh) planOrder.push_back(mk);
        return it->second;
    };
    auto hasC = [&](int fi) { return recs.collision.count(fi) != 0; };

    for (const NodeRecord& nr : recs.nodes)
    {
        // The node's parts grouped by the Mesh owning each (first-part order).
        std::vector<uint64_t> order;
        for (int fi : nr.meshFileIndices)
        {
            if (fi < 0 || static_cast<size_t>(fi) >= fl.size()) continue;
            const uint64_t mk = fl[static_cast<size_t>(fi)].meshKey;
            if (mk == 0)
            {
                if (hasC(fi)) planOf(0).refuse = "the part's Mesh is unknown (its library has no identity)";
                continue;
            }
            if (std::find(order.begin(), order.end(), mk) == order.end()) order.push_back(mk);
        }
        for (uint64_t mk : order)
        {
            std::vector<int> parts;
            for (int fi : nr.meshFileIndices)
                if (fi >= 0 && static_cast<size_t>(fi) < fl.size() && fl[static_cast<size_t>(fi)].meshKey == mk)
                    parts.push_back(fi);
            MeshPlan& plan = planOf(mk);
            if (!plan.id)
            {
                const ModelFLine& f0 = fl[static_cast<size_t>(parts.front())];
                plan.id    = f0.id;
                plan.owner = f0.id ? f0.id->sections[static_cast<size_t>(f0.entry)].ownerMesh : UINT32_MAX;
            }
            ++plan.placements;
            if (!plan.refuse.empty()) continue;

            std::vector<Resource::CollisionPrim> body;
            std::vector<std::string>             sig;
            for (size_t k = 0; k < parts.size() && plan.refuse.empty(); ++k)
            {
                const int fi = parts[k];
                const ModelFLine& f = fl[static_cast<size_t>(fi)];
                uint32_t ownTrimeshes = 0;
                const auto cit = recs.collision.find(fi);
                if (cit != recs.collision.end())
                    for (const CollisionRecord& cr : cit->second)
                    {
                        if (plan.layer < 0) plan.layer = static_cast<int>(cr.layer);
                        if (plan.trigger < 0) plan.trigger = cr.isTrigger ? 1 : 0;
                        if (plan.layer != static_cast<int>(cr.layer) || plan.trigger != (cr.isTrigger ? 1 : 0))
                        {
                            plan.refuse = "its primitives' layers / triggers differ";
                            break;
                        }
                        Resource::CollisionPrim p;
                        bool own = false;
                        std::string why;
                        if (!PrimFromCRecord(cr, f, iscnDir, p, own, why)) { plan.refuse = why; break; }
                        if (own) { ++ownTrimeshes; continue; }
                        if (k != 0) { plan.refuse = "a body primitive on a part other than the Mesh's first"; break; }
                        sig.push_back(PrimSignature(p));
                        body.push_back(std::move(p));
                    }
                if (!plan.refuse.empty()) break;
                if (ownTrimeshes > 1) { plan.refuse = "several trimeshes of one section"; break; }
                if (k == 0 && ownTrimeshes && !body.empty())
                {
                    plan.refuse = "a body and its own section's trimesh on one part";
                    break;
                }
                const bool has = ownTrimeshes == 1;
                const auto [tIt, freshSec] = plan.trimesh.try_emplace(f.secKey, has);
                if (freshSec) plan.trimeshOrder.push_back(f.secKey);
                else if (tIt->second != has)
                    plan.refuse = "placements disagree on section " + Resource::MeshKeyToHex(f.secKey) + "'s trimesh";
            }
            if (!plan.refuse.empty()) continue;
            if (!plan.seen)
            {
                plan.seen    = true;
                plan.bodySig = sig;
                plan.body    = std::move(body);
            }
            else if (plan.bodySig != sig)
                plan.refuse = "placements disagree on the body primitives";
        }
    }

    // ---- The records: BO / PR per migrated Mesh; its C lines go ----
    std::vector<bool> dropLine(lines.size(), false);
    std::string       added;
    std::unordered_map<uint64_t, bool> migratedMesh;
    for (uint64_t mk : planOrder)
    {
        MeshPlan& plan = plans[mk];
        const bool anyTrimesh = std::any_of(plan.trimesh.begin(), plan.trimesh.end(),
                                            [](const auto& kv) { return kv.second; });
        if (plan.refuse.empty() && plan.body.empty() && !anyTrimesh) continue;   // placed, never collides
        if (plan.refuse.empty() && existing.Find(mk)) plan.refuse = "the Mesh already has a BO record";
        if (plan.refuse.empty() && mk == 0) plan.refuse = "the part's Mesh is unknown";
        if (!plan.refuse.empty())
        {
            rep.refused.emplace_back(mk, plan.refuse);
            continue;
        }

        Resource::MeshBodySetup setup;
        setup.meshKey = mk;
        setup.layer   = static_cast<uint8_t>(plan.layer < 0 ? 0 : plan.layer);
        setup.trigger = plan.trigger == 1;
        setup.prims   = plan.body;
        // Every section of the Mesh (its identity's list) with a trimesh = complex.
        bool complex = anyTrimesh && plan.body.empty() && plan.id && plan.owner < plan.id->meshes.size();
        if (complex)
            for (uint32_t e : plan.id->meshes[plan.owner].sections)
            {
                const uint64_t sk = e < plan.id->sections.size() ? plan.id->sections[e].key : 0;
                const auto it = plan.trimesh.find(sk);
                if (it == plan.trimesh.end() || !it->second) { complex = false; break; }
            }
        if (complex)
        {
            setup.complexity = Resource::MeshBodySetup::Complexity::Complex;
            ++rep.complexMeshes;
        }
        else
        {
            // Own-section trimeshes as PR type=mesh, in the identity's section order.
            std::vector<uint64_t> secs;
            if (plan.id && plan.owner < plan.id->meshes.size())
                for (uint32_t e : plan.id->meshes[plan.owner].sections)
                    if (e < plan.id->sections.size())
                        if (const auto it = plan.trimesh.find(plan.id->sections[e].key); it != plan.trimesh.end() && it->second)
                            secs.push_back(it->first);
            for (uint64_t sk : plan.trimeshOrder)   // any the identity's list did not name
                if (plan.trimesh[sk] && std::find(secs.begin(), secs.end(), sk) == secs.end()) secs.push_back(sk);
            for (uint64_t sk : secs)
            {
                Resource::CollisionPrim p;
                p.type       = Resource::CollisionPrim::Type::Mesh;
                p.sectionKey = sk;
                setup.prims.push_back(p);
            }
            rep.sectionPrims += static_cast<uint32_t>(secs.size());
        }
        rep.bodyPrims += static_cast<uint32_t>(plan.body.size());
        if (plan.body.size() > 1)
        {
            ++rep.compounds;
            rep.colEntities += plan.placements * static_cast<uint32_t>(plan.body.size() - 1);
        }
        ++rep.meshes;
        migratedMesh[mk] = true;

        // FormatBodySetup ends each line in "\n": the file's own ending instead.
        const std::string t = Resource::FormatBodySetup(setup);
        for (size_t b = 0; b < t.size();)
        {
            const size_t e = t.find('\n', b);
            added += t.substr(b, (e == std::string::npos ? t.size() : e) - b);
            added += eol;
            if (e == std::string::npos) break;
            b = e + 1;
        }
    }
    for (const auto& [fi, lineNos] : recs.collisionLine)
    {
        if (fi < 0 || static_cast<size_t>(fi) >= fl.size()) continue;
        if (!migratedMesh.count(fl[static_cast<size_t>(fi)].meshKey)) continue;
        for (size_t ln : lineNos)
        {
            dropLine[ln] = true;
            ++rep.migrated;
        }
    }
    if (added.empty()) return true;

    // The records go before the ME block, which StampModelKeys keeps as the file's
    // last (the importer writes N, F, collision, then the stamp appends ME) - after
    // it, the next key stamp would move them and report the model as unkeyed.
    size_t meAt = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (!dropLine[i] && lines[i].size() > 3 && lines[i][0] == 'M' && lines[i][1] == 'E' && lines[i][2] == ' ')
        {
            meAt = i;
            break;
        }
    std::string rebuilt;
    rebuilt.reserve(text.size() + added.size());
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i == meAt)
        {
            if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
            rebuilt += added;
        }
        if (dropLine[i]) continue;
        rebuilt += lines[i];
        rebuilt += eols[i];
    }
    if (meAt == lines.size())
    {
        if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
        rebuilt += added;
    }

    rep.changed = (rebuilt != text);
    if (!apply || !rep.changed) return true;
    if (!WriteIscnText(iscnPath, blob, rebuilt, "SceneInstanceLoader::MigrateModelCollision"))
        return fail("cannot write '" + iscnPath + "'");
    rep.written = true;
    return true;
}

bool SceneInstanceLoader::ReadPartColliderDefaults(const std::string& iscnPath, std::vector<PartColliderDefault>& out,
                                                   LibraryIdentityCache& libs)
{
    out.clear();
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob) || blob.empty() ||
        !Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
        return false;
    std::string text;
    {
        const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        text.assign(payl, n);
    }
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string        line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    ModelRecords recs;
    ParseModelRecords(lines, recs);
    const fs::path iscnDir = Sys::U8Path(iscnPath).parent_path();
    const std::vector<ModelFLine> fl = ResolveModelFLines(recs.files, iscnDir, libs);
    for (size_t fi = 0; fi < fl.size(); ++fi)
    {
        const ModelFLine& f = fl[fi];
        // Not a .meshlib entry (a per-mesh file, a library that does not read, an
        // entry out of range): the spawn makes no part of it either.
        if (!f.id || f.entry < 0) continue;
        PartColliderDefault row;
        row.lib   = f.lib;
        row.entry = static_cast<uint32_t>(f.entry);
        if (const auto it = recs.collision.find(static_cast<int>(fi)); it != recs.collision.end())
            for (const CollisionRecord& cr : it->second)
                if (MakeColliderFromRecord(cr, f.lib, row.entry, iscnDir, row.collider))
                {
                    row.hasCollider = true;   // the FIRST accepted record rides on the part
                    break;
                }
        out.push_back(std::move(row));
    }
    return true;
}

// ==== Editing a model's per-Mesh records (asset collision, shape keys; see the header) ====
namespace
{
    // The MeshKey a record with prefix @p a or @p b names (0: none / another record).
    uint64_t RecordMeshKey(const std::string& line, const char* a, const char* b)
    {
        if (line.size() < 4 || line[2] != ' ' ||
            !((line[0] == a[0] && line[1] == a[1]) || (line[0] == b[0] && line[1] == b[1])))
            return 0;
        const size_t at = line.find(" mesh=");
        if (at == std::string::npos) return 0;
        const size_t s = at + 6, e = line.find(' ', s);
        uint64_t k = 0;
        return Resource::MeshKeyFromHex(line.substr(s, e == std::string::npos ? std::string::npos : e - s), k) ? k : 0;
    }

    // The records @p drop says go (it sees each line without its line end) are
    // replaced by @p addedLf (lines ending in "\n", given the file's own line
    // ends), placed before the ME block (StampModelKeys keeps it last - see
    // MigrateModelCollision); the file is written (temp + rename) only when the
    // text changes.
    template <typename Drop>
    bool ReplaceRecordsIf(const std::string& iscnPath, Drop&& drop, const std::string& addedLf,
                          std::string& outOldText, std::string& outNewText, std::string* outError,
                          const char* who)
    {
        auto fail = [&](std::string why)
        {
            if (outError) *outError = why;
            return false;
        };
        std::vector<uint8_t> blob;
        std::string          text;
        if (!ReadIscnBlobForEdit(iscnPath, blob, text, who))
            return fail("cannot read '" + iscnPath + "' as a model");
        outOldText = text;

        std::string eol = "\n";
        if (const size_t nl = text.find('\n'); nl != std::string::npos && nl > 0 && text[nl - 1] == '\r') eol = "\r\n";
        std::string added;
        for (size_t p = 0; p < addedLf.size();)
        {
            const size_t e = addedLf.find('\n', p);
            added += addedLf.substr(p, (e == std::string::npos ? addedLf.size() : e) - p);
            added += eol;
            if (e == std::string::npos) break;
            p = e + 1;
        }

        std::string rebuilt;
        rebuilt.reserve(text.size() + added.size());
        bool inserted = false;
        for (size_t p = 0; p < text.size();)
        {
            const size_t e = text.find('\n', p);
            const std::string raw = text.substr(p, e == std::string::npos ? std::string::npos : e + 1 - p);
            p = (e == std::string::npos) ? text.size() : e + 1;
            std::string line = raw;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!inserted && line.size() > 3 && line[0] == 'M' && line[1] == 'E' && line[2] == ' ')
            {
                if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
                rebuilt += added;
                inserted = true;
            }
            if (drop(line)) continue;
            rebuilt += raw;
        }
        if (!inserted)
        {
            if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
            rebuilt += added;
        }
        outNewText = rebuilt;
        if (rebuilt == text) return true;
        if (!WriteIscnText(iscnPath, blob, rebuilt, who))
            return fail("cannot write '" + iscnPath + "'");
        return true;
    }

    // The records with prefix @p a / @p b that name one of @p keys (by mesh=).
    bool ReplaceMeshRecords(const std::string& iscnPath, const char* a, const char* b,
                            const std::vector<uint64_t>& keys, const std::string& addedLf,
                            std::string& outOldText, std::string& outNewText, std::string* outError,
                            const char* who)
    {
        auto drop = [&](const std::string& line)
        {
            const uint64_t mk = RecordMeshKey(line, a, b);
            return mk != 0 && std::find(keys.begin(), keys.end(), mk) != keys.end();
        };
        return ReplaceRecordsIf(iscnPath, drop, addedLf, outOldText, outNewText, outError, who);
    }
}

bool SceneInstanceLoader::ReplaceModelCollision(const std::string& iscnPath,
                                                const std::vector<Resource::MeshBodySetup>& setups,
                                                std::string& outOldText, std::string& outNewText,
                                                std::string* outError)
{
    std::vector<uint64_t> keys;
    std::string           added;
    for (const Resource::MeshBodySetup& s : setups)
    {
        keys.push_back(s.meshKey);
        if (!s.Empty()) added += Resource::FormatBodySetup(s);   // Empty: its records go, none come
    }
    return ReplaceMeshRecords(iscnPath, "BO", "PR", keys, added, outOldText, outNewText, outError,
                              "SceneInstanceLoader::ReplaceModelCollision");
}

bool SceneInstanceLoader::ReplaceModelShapeKeys(const std::string& iscnPath,
                                                const std::vector<Resource::MeshShapeKeySetup>& setups,
                                                std::string& outOldText, std::string& outNewText,
                                                std::string* outError)
{
    std::vector<uint64_t> keys;
    std::string           added;
    for (const Resource::MeshShapeKeySetup& s : setups)
    {
        keys.push_back(s.meshKey);
        if (!s.Empty()) added += Resource::FormatShapeKeys(s);
    }
    return ReplaceMeshRecords(iscnPath, "MK", "MP", keys, added, outOldText, outNewText, outError,
                              "SceneInstanceLoader::ReplaceModelShapeKeys");
}

// ==== The model's sockets and virtual bones (see the header) ====
bool SceneInstanceLoader::ReplaceModelSockets(const std::string& iscnPath,
                                              const std::vector<Resource::ModelSocket>& sockets,
                                              std::string& outOldText, std::string& outNewText,
                                              std::string* outError)
{
    for (const Resource::ModelSocket& s : sockets)
        if (const std::string why = Resource::SocketNameProblem(s.name); !why.empty())
        {
            if (outError) *outError = why;
            return false;
        }
    auto drop = [](const std::string& line) { return line.size() > 3 && line[0] == 'S' && line[1] == 'O' && line[2] == ' '; };
    return ReplaceRecordsIf(iscnPath, drop, Resource::FormatSockets(sockets), outOldText, outNewText, outError,
                            "SceneInstanceLoader::ReplaceModelSockets");
}

bool SceneInstanceLoader::ReplaceModelVirtualBones(const std::string& iscnPath,
                                                   const std::vector<Resource::ModelVirtualBone>& bones,
                                                   std::string& outOldText, std::string& outNewText,
                                                   std::string* outError)
{
    for (const Resource::ModelVirtualBone& v : bones)
        if (const std::string why = Resource::VirtualBoneNameProblem(v.name); !why.empty() || v.from.empty() || v.to.empty())
        {
            if (outError) *outError = why.empty() ? "virtual bone '" + v.name + "' needs a from and a to bone" : why;
            return false;
        }
    auto drop = [](const std::string& line) { return line.size() > 3 && line[0] == 'V' && line[1] == 'B' && line[2] == ' '; };
    return ReplaceRecordsIf(iscnPath, drop, Resource::FormatVirtualBones(bones), outOldText, outNewText, outError,
                            "SceneInstanceLoader::ReplaceModelVirtualBones");
}

bool SceneInstanceLoader::ReplaceModelSkeletonRecords(const std::string& iscnPath,
                                                      const std::vector<Resource::ModelSocket>& sockets,
                                                      const std::vector<Resource::ModelVirtualBone>& bones,
                                                      std::string& outOldText, std::string& outNewText,
                                                      std::string* outError)
{
    auto fail = [&](const std::string& why)
    {
        if (outError) *outError = why;
        return false;
    };
    for (const Resource::ModelSocket& s : sockets)
        if (const std::string why = Resource::SocketNameProblem(s.name); !why.empty()) return fail(why);
    for (const Resource::ModelVirtualBone& v : bones)
        if (const std::string why = Resource::VirtualBoneNameProblem(v.name); !why.empty() || v.from.empty() || v.to.empty())
            return fail(why.empty() ? "virtual bone '" + v.name + "' needs a from and a to bone" : why);
    // One write: the virtual bones first (the bones the sockets may name), then the
    // skeleton sockets. A mesh socket line (`SO mesh=`) belongs to the Mesh Editor:
    // kept as the file has it, never written from @p sockets.
    auto drop = [](const std::string& line)
    {
        if (line.size() <= 3 || line[2] != ' ') return false;
        if (line[0] == 'V' && line[1] == 'B') return true;
        return line[0] == 'S' && line[1] == 'O' && line.find(" mesh=") == std::string::npos;
    };
    std::vector<Resource::ModelSocket> skeletonSockets;
    for (const Resource::ModelSocket& s : sockets)
        if (s.IsSkeletonSocket()) skeletonSockets.push_back(s);
    return ReplaceRecordsIf(iscnPath, drop, Resource::FormatVirtualBones(bones) + Resource::FormatSockets(skeletonSockets),
                            outOldText, outNewText, outError, "SceneInstanceLoader::ReplaceModelSkeletonRecords");
}

bool SceneInstanceLoader::ReplaceMeshSockets(const std::string& iscnPath, uint64_t meshKey,
                                             const std::vector<Resource::ModelSocket>& sockets,
                                             std::string& outOldText, std::string& outNewText, std::string* outError)
{
    auto fail = [&](const std::string& why)
    {
        if (outError) *outError = why;
        return false;
    };
    if (meshKey == 0) return fail("no Mesh key");
    for (const Resource::ModelSocket& s : sockets)
    {
        if (const std::string why = Resource::SocketNameProblem(s.name); !why.empty()) return fail(why);
        if (s.meshKey != meshKey)
            return fail("socket '" + s.name + "' is not a socket of the Mesh " + Resource::MeshKeyToHex(meshKey));
    }
    // This Mesh's lines only: the key as the record spells it (FormatSocket's mesh=<hex>).
    const std::string key = " mesh=" + Resource::MeshKeyToHex(meshKey);
    auto drop = [&key](const std::string& line)
    {
        if (line.size() <= 3 || line[0] != 'S' || line[1] != 'O' || line[2] != ' ') return false;
        const std::size_t at = line.find(key);
        return at != std::string::npos &&
               (at + key.size() == line.size() || line[at + key.size()] == ' ' || line[at + key.size()] == '\r');
    };
    return ReplaceRecordsIf(iscnPath, drop, Resource::FormatSockets(sockets), outOldText, outNewText, outError,
                            "SceneInstanceLoader::ReplaceMeshSockets");
}

uint32_t SceneInstanceLoader::DeriveMeshSockets(World& world, Entity owner, const Resource::ModelSockets& sockets,
                                                uint64_t meshKey, const std::string& modelPath)
{
    // As DeriveModelSockets: MAX sockets x ~280 B on the heap.
    auto list = std::make_unique<SocketComponent::Socket[]>(SocketComponent::MAX);
    uint32_t n = 0, over = 0;
    if (meshKey != 0)
        for (const Resource::ModelSocket& ms : sockets.sockets)
        {
            if (ms.meshKey != meshKey) continue;
            if (n >= SocketComponent::MAX) { ++over; continue; }
            SocketComponent::Socket& s = list[n++];
            const size_t nn = (std::min)(ms.name.size(), sizeof(s.name) - 1);
            std::memcpy(s.name, ms.name.data(), nn);
            s.name[nn] = '\0';
            s.boneIndex        = 0;           // no bone: SocketSystem places it on the entity
            s.localOffset      = ms.LocalOffset();
            s.rotationEulerDeg = MathUtils::RollPitchYawDegreesFromQuaternion(ms.q);
            s.flags            = SocketComponent::kDerived | SocketComponent::kMeshSocket;
        }
    if (over > 0)
        LOG_WARNING("SceneInstanceLoader: '%s' - %u mesh socket(s) of Mesh %s past SocketComponent::MAX (%u), not "
                    "published", modelPath.c_str(), over, Resource::MeshKeyToHex(meshKey).c_str(), SocketComponent::MAX);
    SocketComponent* sc = world.GetComponent<SocketComponent>(owner);
    if (!sc)
    {
        if (n == 0) return 0;   // nothing to publish: no component for nothing
        world.AddComponent<SocketComponent>(owner, *std::make_unique<SocketComponent>());
        sc = world.GetComponent<SocketComponent>(owner);
        if (!sc) return 0;
    }
    const uint32_t taken = sc->ReplaceDerived(list.get(), n, SocketComponent::kMeshSocket);
    if (n > 0)
        LOG_INFO("SceneInstanceLoader: '%s' Mesh %s - %u mesh socket(s) on entity %u (%u not taken: the entity's own "
                 "of that name wins, or past MAX)", modelPath.c_str(), Resource::MeshKeyToHex(meshKey).c_str(), taken,
                 owner, n - taken);
    return taken;
}

uint32_t SceneInstanceLoader::CountStaleVirtualBones(World& world, const SkeletonRegistry& skeletons,
                                                     const std::string& iscnPath)
{
    const std::string want = Resource::AssetRefs::NormalizeRefPath(iscnPath);
    const uint64_t    sig  = Resource::VirtualBonesSignature(
        Resource::ModelVirtualBonesAt(Sys::RelativizeAssetPath(iscnPath))->bones);   // re-read when the file changed
    std::vector<Entity> roots;
    world.ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        if (Resource::AssetRefs::NormalizeRefPath(s.path) == want) roots.push_back(e);
    });
    uint32_t stale = 0;
    for (Entity root : roots)
    {
        // A panel's preview stage is not a placement (its owner respawns it
        // itself).
        if (world.HasComponent<PreviewStageTag>(root)) continue;
        const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(root);
        if (!sk || sk->assetIndex == kInvalidSkeletonIndex || sk->assetIndex >= skeletons.Count()) continue;
        if (skeletons.Get(sk->assetIndex).virtualBoneSignature != sig) ++stale;
    }
    return stale;
}

uint32_t SceneInstanceLoader::DeriveModelSockets(World& world, Entity root, const SkeletonAsset& skel,
                                                 const Resource::ModelSockets& sockets, const std::string& modelPath)
{
    // 32 sockets x ~280 B: on the heap - Load runs deep in the main thread's stack.
    auto list = std::make_unique<SocketComponent::Socket[]>(SocketComponent::MAX);
    uint32_t    n = 0, over = 0, orphans = 0;
    std::string missing;
    for (const Resource::ModelSocket& ms : sockets.sockets)
    {
        if (!ms.IsSkeletonSocket()) continue;   // a mesh socket: MeshInstanceSystem publishes it (DeriveMeshSockets)
        uint32_t bone = ~0u;
        for (uint32_t b = 0; b < skel.boneCount && bone == ~0u; ++b)
            if (std::strncmp(skel.boneNames[b], ms.bone.c_str(), sizeof(skel.boneNames[b])) == 0) bone = b;
        if (bone == ~0u)
        {
            // An orphan (a re-import lost its bone and said so then): counted, not warned again.
            if (ms.orphan) ++orphans;
            else missing += (missing.empty() ? "'" : ", '") + ms.name + "' (bone '" + ms.bone + "')";
            continue;
        }
        if (n >= SocketComponent::MAX) { ++over; continue; }
        SocketComponent::Socket& s = list[n++];
        const size_t nn = (std::min)(ms.name.size(), sizeof(s.name) - 1);
        std::memcpy(s.name, ms.name.data(), nn);
        s.name[nn] = '\0';
        const size_t bn = (std::min)(ms.bone.size(), sizeof(s.boneName) - 1);
        std::memcpy(s.boneName, ms.bone.data(), bn);
        s.boneName[bn] = '\0';
        s.boneIndex        = bone;
        s.localOffset      = ms.LocalOffset();
        s.rotationEulerDeg = MathUtils::RollPitchYawDegreesFromQuaternion(ms.q);
        s.flags            = SocketComponent::kDerived;
    }
    if (!missing.empty())
        LOG_WARNING("SceneInstanceLoader: '%s' - socket(s) naming a bone the skeleton does not have, not published: %s",
                    modelPath.c_str(), missing.c_str());
    if (over > 0)
        LOG_WARNING("SceneInstanceLoader: '%s' - %u socket(s) past SocketComponent::MAX (%u), not published",
                    modelPath.c_str(), over, SocketComponent::MAX);

    SocketComponent* sc = world.GetComponent<SocketComponent>(root);
    if (!sc)
    {
        if (n == 0) return 0;   // nothing to publish: no component for nothing
        world.AddComponent<SocketComponent>(root, *std::make_unique<SocketComponent>());
        sc = world.GetComponent<SocketComponent>(root);
        if (!sc) return 0;
    }
    const uint32_t taken = sc->ReplaceDerived(list.get(), n);
    LOG_INFO("SceneInstanceLoader: '%s' - %u model socket(s) on entity %u (%u not taken: the entity's own of that "
             "name wins, or past MAX; %u orphan(s) skipped)", modelPath.c_str(), taken, root, n - taken, orphans);
    return taken;
}

uint32_t SceneInstanceLoader::RefreshModelSockets(World& world, const SkeletonRegistry& skeletons,
                                                  const std::string& iscnPath)
{
    const std::string want    = Resource::AssetRefs::NormalizeRefPath(iscnPath);
    const std::string model   = Sys::RelativizeAssetPath(iscnPath);
    const auto        sockets = Resource::ModelSocketsAt(model);   // re-read when the file changed

    std::vector<Entity> roots;
    world.ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        if (Resource::AssetRefs::NormalizeRefPath(s.path) == want) roots.push_back(e);
    });
    uint32_t refreshed = 0;
    for (Entity root : roots)
    {
        const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(root);
        if (!sk || sk->assetIndex == kInvalidSkeletonIndex || sk->assetIndex >= skeletons.Count()) continue;
        DeriveModelSockets(world, root, skeletons.Get(sk->assetIndex), *sockets, model);
        ++refreshed;
    }
    LOG_INFO("SceneInstanceLoader::RefreshModelSockets: '%s' - %u of %zu placement(s) refreshed (%zu socket record(s))",
             model.c_str(), refreshed, roots.size(), sockets->SkeletonSocketCount());
    return refreshed;
}

bool SceneInstanceLoader::WriteModelText(const std::string& iscnPath, const std::string& text, std::string* outError)
{
    std::vector<uint8_t> blob;
    std::string          current;
    if (!ReadIscnBlobForEdit(iscnPath, blob, current, "SceneInstanceLoader::WriteModelText"))
    {
        if (outError) *outError = "cannot read '" + iscnPath + "' as a model";
        return false;
    }
    if (current == text) return true;
    if (!WriteIscnText(iscnPath, blob, text, "SceneInstanceLoader::WriteModelText"))
    {
        if (outError) *outError = "cannot write '" + iscnPath + "'";
        return false;
    }
    return true;
}

bool SceneInstanceLoader::ModelHasCRecords(const std::string& iscnPath)
{
    std::vector<uint8_t> blob;
    std::string          text;
    if (!ReadIscnBlobForEdit(iscnPath, blob, text, "SceneInstanceLoader::ModelHasCRecords")) return false;
    for (size_t b = 0; b < text.size();)
    {
        if (b + 1 < text.size() && text[b] == 'C' && text[b + 1] == ' ') return true;
        const size_t e = text.find('\n', b);
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return false;
}

uint32_t SceneInstanceLoader::RefreshAssetCollision(World& world, const std::string& iscnPath)
{
    const std::string want  = Resource::AssetRefs::NormalizeRefPath(iscnPath);
    const std::string model = Sys::RelativizeAssetPath(iscnPath);
    const auto        mc    = Resource::ModelCollisionAt(model);   // re-read when the file changed
    LibraryIdentityCache libs;
    AssetCollisionSpawn  counts;

    std::vector<Entity> roots;
    world.ForEach<SceneSourcePath>([&](Entity e, SceneSourcePath& s)
    {
        if (Resource::AssetRefs::NormalizeRefPath(s.path) == want) roots.push_back(e);
    });

    uint32_t changed = 0;
    std::vector<Entity> stack;
    for (Entity root : roots)
    {
        stack.assign(1, root);
        while (!stack.empty())
        {
            const Entity n = stack.back();
            stack.pop_back();
            const Children* ch = world.GetComponent<Children>(n);
            if (!ch) continue;
            // This node's parts, in its child order (as the spawn made them).
            std::vector<AssetPart> parts;
            for (Entity c : ch->entities)
            {
                stack.push_back(c);
                const MeshLibRef*     ref = world.GetComponent<MeshLibRef>(c);
                const MeshSourcePath* sp  = world.GetComponent<MeshSourcePath>(c);
                if (!ref || !sp || sp->path.empty()) continue;
                AssetPart p;
                p.ent   = c;
                p.lib   = sp->path;
                p.entry = ref->meshId;
                if (const Resource::MeshIdentity* id = libs.Get(sp->path); id && ref->meshId < id->sections.size())
                {
                    const Resource::MeshIdentity::Section& s = id->sections[ref->meshId];
                    p.sectionKey = s.key;
                    if (s.ownerMesh < id->meshes.size()) p.meshKey = id->meshes[s.ownerMesh].key;
                }
                parts.push_back(std::move(p));
            }
            if (parts.empty()) continue;
            // What the asset gave these parts, unchanged since, goes...
            for (const AssetPart& p : parts)
            {
                bool fromAsset = false;
                if (!Resource::ColliderIsSpawnDefault(world, p.ent, &fromAsset) || !fromAsset) continue;
                world.RemoveComponent<ColliderComponent>(p.ent);
                world.RemoveComponent<RigidBodyComponent>(p.ent);
                world.RemoveComponent<ColliderSpawnDefault>(p.ent);
                ++changed;
            }
            // ...and the model's current asset collision comes, as the spawn gives it.
            std::vector<Entity> given;
            GiveAssetCollision(world, *mc, model, parts, counts, given, nullptr);
            for (Entity e : given)
            {
                const ColliderComponent*  col = world.GetComponent<ColliderComponent>(e);
                const RigidBodyComponent* rb  = world.GetComponent<RigidBodyComponent>(e);
                if (col && rb) world.AddComponent<ColliderSpawnDefault>(e, ColliderSpawnDefault{ *col, *rb, true });
                ++changed;
            }
        }
    }
    LOG_INFO("SceneInstanceLoader::RefreshAssetCollision: '%s' - %zu placement(s), %u part collider change(s) "
             "(%u Mesh body(ies), %u section trimesh(es), %u part(s) kept an edited collider)",
             model.c_str(), roots.size(), changed, counts.meshBodies, counts.sectionBodies, counts.conflicts);
    return changed;
}

// ==== The read-only placement view: ReadMeshPlacements and its helpers (see the header) ====
// ReadMeshPlacements walks the spawn's child order (WalkNodeChildSlots, shared with
// SpawnSingleNode), so a DFS slot is consumed exactly where the spawn creates an
// entity. The one residual (see its slotMesh): a library that opens but does not
// parse, or a meshId past its end, is counted there but skipped by the spawn; the
// `-preview_asset_verify` harness reports that difference as non-zero
// `mismatches`.
bool SceneInstanceLoader::NamesSkeleton(const std::string& iscnPath)
{
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob) || blob.empty() ||
        !Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
        return false;
    const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
    const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
    const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
    size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
    if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
    for (size_t b = 0; b < n;)
    {
        if (b + 1 < n && payl[b] == 'K' && payl[b + 1] == ' ') return true;
        const void* nl = std::memchr(payl + b, '\n', n - b);
        if (!nl) break;
        b = static_cast<size_t>(static_cast<const char*>(nl) - payl) + 1;
    }
    return false;
}

uint64_t SceneInstanceLoader::PlacementSectionKey(const MeshPlacement& mp)
{
    const auto t = Resource::MeshLibKeyTableAt(mp.meshPath);
    if (!t) return 0;
    if (mp.sectionKey != 0 && t->Find(mp.sectionKey) >= 0) return mp.sectionKey;
    return t->KeyOf(mp.meshId);
}

bool SceneInstanceLoader::ReadMeshPlacements(const std::string&          iscnPath,
                                             std::vector<MeshPlacement>& out)
{
    out.clear();

    // AssetFS, not Sys: reading works on a packed build too (as in
    // ReadMaterialBindings).
    std::vector<uint8_t> blob;
    if (!::Resource::AssetFS::Get().ReadFile(iscnPath, blob) || blob.empty())
    {
        LOG_WARNING("SceneInstanceLoader::ReadMeshPlacements: cannot open '%s'",
                    iscnPath.c_str());
        return false;
    }
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
    {
        LOG_WARNING("SceneInstanceLoader::ReadMeshPlacements: bad header in '%s'",
                    iscnPath.c_str());
        return false;
    }

    std::string text;
    {
        const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        const void*  nul  = std::memchr(payl, 0, n);
        if (nul) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        text.assign(payl, n);
    }

    std::vector<NodeRecord> nodes;
    std::vector<FileRecord> files;
    // C and V records are parsed here too: the entities they spawn occupy DFS slots
    // between one mesh entity and the next, so ignoring them would mis-key every
    // later override.
    std::unordered_map<int, std::vector<CollisionRecord>> collisionRecords;
    std::unordered_map<int, std::vector<CollisionRecord>> volumeRecords;
    {
        std::istringstream ss(text);
        std::string        line;
        while (std::getline(ss, line))
        {
            if (line.size() < 2 || line[0] == '#') continue;
            if (line[0] == 'N' && line[1] == ' ')
            {
                NodeRecord nr;
                if (ParseNLine(line, nr)) nodes.push_back(std::move(nr));
            }
            else if (line[0] == 'F' && line[1] == ' ')
            {
                FileRecord fr;
                if (ParseFLine(line, fr)) files.push_back(std::move(fr));
            }
            else if (line[0] == 'C' && line[1] == ' ')
            {
                CollisionRecord cr;
                if (ParseCLine(line, cr) && cr.fileIdx >= 0)
                    collisionRecords[cr.fileIdx].push_back(std::move(cr));
            }
            else if (line[0] == 'V' && line[1] == ' ')
            {
                CollisionRecord cr;
                if (ParseCLine(line, cr) && cr.nodeIdx >= 0)
                    volumeRecords[cr.nodeIdx].push_back(std::move(cr));
            }
        }
    }
    if (nodes.empty()) return true;

    const fs::path iscnDir = Sys::U8Path(iscnPath).parent_path();

    // idx -> position in `nodes`, and the child lists, both keyed by the
    // AUTHORED idx rather than by position: nothing guarantees the N lines are
    // written in idx order and ParseNLine does not enforce it.
    std::unordered_map<int, size_t> byIdx;
    for (size_t i = 0; i < nodes.size(); ++i) byIdx[nodes[i].idx] = i;

    std::unordered_map<int, std::vector<int>> childrenOf;
    std::vector<int> roots;
    // parent == -1 is the root test, as Load's spawn loop spells it. A node naming
    // a parent that does not exist is not a root there (it is never reached), so it
    // must not become one here either.
    for (const NodeRecord& nr : nodes)
    {
        if (nr.parent >= 0) childrenOf[nr.parent].push_back(nr.idx);
        else                roots.push_back(nr.idx);
    }
    // Ascending: the order Load's DFS spawns children in, hence the order of the
    // Children vector.
    for (auto& kv : childrenOf) std::sort(kv.second.begin(), kv.second.end());
    std::sort(roots.begin(), roots.end());

    // ---- One root, exactly as LoadPrefab sees it ----
    // SceneInstanceLoader::Load returns the FIRST root it spawned, and LoadPrefab
    // runs CollectSubtreeDFS from that entity, so a second root's subtree is not in
    // the index the overrides go through. Counting it would shift nothing (it comes
    // last) but would invent placements the prefab has no override slot for: it is
    // dropped, with one warning.
    if (roots.size() > 1)
    {
        static bool s_multiRootLogged = false;
        if (!s_multiRootLogged)
        {
            s_multiRootLogged = true;
            LOG_WARNING("SceneInstanceLoader::ReadMeshPlacements: '%s' has %zu root "
                        "nodes; only the FIRST is read, because that is the only "
                        "one PrefabSerializer::LoadPrefab keys its per-part "
                        "overrides through (logged once per session)",
                        iscnPath.c_str(), roots.size());
        }
    }
    if (roots.empty()) return true;
    const int rootIdx = roots.front();

    auto localMatrix = [](const NodeRecord& nr) -> XMMATRIX
    {
        return XMMatrixAffineTransformation(
            XMVectorSet(nr.sx, nr.sy, nr.sz, 0.f),
            XMVectorZero(),
            XMVectorSet(nr.qx, nr.qy, nr.qz, nr.qw),
            XMVectorSet(nr.tx, nr.ty, nr.tz, 0.f));
    };

    // The expression Load fills meshAbsPaths[] with, so a collider record's
    // fallback geometry resolves to the same string and MakeColliderFromRecord
    // answers the same way on both sides.
    auto meshAbsFor = [&](int fi) -> std::string
    {
        if (fi < 0 || static_cast<size_t>(fi) >= files.size()) return {};
        return Sys::PathToUtf8(iscnDir / Sys::U8Path(files[static_cast<size_t>(fi)].file));
    };

    // Pre-order, iterative, like Load's DFS and for the same reason: a level import
    // is thousands of nodes deep in the worst case.
    struct Frame { int idx; XMMATRIX parentWorld; bool isRoot; };
    std::vector<Frame> stack;
    stack.push_back({ rootIdx, XMMatrixIdentity(), true });

    uint32_t dfs = 0;
    // Cycle guard: a hand-edited .iscn can name itself its own parent, and the
    // walk would then never terminate.
    std::unordered_map<int, bool> visited;
    // "Can AssetFS open this F record's mesh file?", memoised per F index for the
    // whole walk (beside `visited`): a level import walks 7054 nodes over 714 F
    // records and a miss is a filesystem stat plus a pak lookup, and the warning in
    // slotMesh fires once per (iscn, F index).
    std::unordered_map<int, bool> meshRefOpens;
    while (!stack.empty())
    {
        const Frame fr = stack.back();
        stack.pop_back();
        auto nit = byIdx.find(fr.idx);
        if (nit == byIdx.end()) continue;
        if (visited[fr.idx]) continue;
        visited[fr.idx] = true;

        const NodeRecord& nr = nodes[nit->second];
        // The root's own local transform is not applied: LoadPrefab
        // overwrites it with the prefab N-line's, and the caller multiplies
        // that in. Every other node composes normally.
        const XMMATRIX world = fr.isRoot
            ? fr.parentWorld
            : XMMatrixMultiply(localMatrix(nr), fr.parentWorld);

        const uint32_t nodeDfs = dfs++;   // the node entity itself
        (void)nodeDfs;

        // The shared child order: every slot SpawnSingleNode would create is
        // visited in the same sequence; only mesh slots produce a placement,
        // the volume / compound-collider slots only consume the DFS indices
        // they take in the live spawn.
        auto slotVolume = [&](const CollisionRecord& cr, int volIdx) -> bool
        {
            ColliderComponent col{};
            if (!MakeColliderFromRecord(cr, std::string(), 0, iscnDir, col))
                return false;
            if (volIdx > 0) ++dfs;   // "_volN" entity
            return true;
        };
        // ---- "Does this F ref become an entity?", agreed with the spawn ----
        // SpawnSingleNode's mesh slot needs a valid fiLibHandle[fi], i.e. a
        // library MeshLibrary::Load produced. A ref counted here but skipped
        // there (a .meshlib deleted or renamed after the .iscn was written)
        // would shift every later dfsIndex by one and silently re-key every
        // prefab override past it.
        // The agreed predicate is "AssetFS can open it": MeshLibrary::Load
        // starts with AssetFS::ReadFile, so a missing file fails on both
        // sides. Residual: a file that opens but fails to parse (bad magic,
        // truncated, wrong version) is counted here and skipped by the spawn;
        // closing that would mean parsing every library in a function meant
        // to answer without loading anything.
        // Memoised per F index (meshRefOpens).
        auto slotMesh = [&](int fi) -> bool
        {
            if (fi < 0 || static_cast<size_t>(fi) >= files.size()) return false;
            const FileRecord& f = files[static_cast<size_t>(fi)];
            if (f.file.empty()) return false;

            MeshPlacement mp;
            // VERBATIM the expression ReadMaterialBindings uses, then
            // project-relativised so the string is what a MeshLibrary::Load
            // caller and an AssetFS reader both accept.
            mp.meshPath = Sys::RelativizeAssetPath(
                Sys::PathToUtf8((iscnDir / Sys::U8Path(f.file)).lexically_normal()));
            mp.meshId   = f.subIdx;
            mp.sectionKey = f.sectionKey;
            mp.fileIndex  = static_cast<uint32_t>(fi);

            auto opensIt = meshRefOpens.find(fi);
            if (opensIt == meshRefOpens.end())
            {
                const bool opens = ::Resource::AssetFS::Get().Exists(mp.meshPath);
                opensIt = meshRefOpens.emplace(fi, opens).first;
                if (!opens)
                    LOG_WARNING("SceneInstanceLoader::ReadMeshPlacements: '%s' F "
                                "record %d names '%s', which AssetFS cannot open - "
                                "the spawn skips it, so this reader skips it too "
                                "(no DFS slot is consumed)",
                                iscnPath.c_str(), fi, mp.meshPath.c_str());
            }
            if (!opensIt->second) return false;
            if (!f.matFile.empty())
                mp.matPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                    (iscnDir / Sys::U8Path(f.matFile)).lexically_normal()));
            mp.dfsIndex = dfs++;   // the mesh entity this ref spawns
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, world);
            std::memcpy(mp.world, &m, sizeof(float) * 16);
            out.push_back(std::move(mp));
            return true;
        };
        auto slotCollider = [&](const CollisionRecord& cr, int fi, int primIdx) -> bool
        {
            ColliderComponent col{};
            const uint32_t meshId =
                (fi >= 0 && static_cast<size_t>(fi) < files.size())
                    ? files[static_cast<size_t>(fi)].subIdx : 0u;
            if (!MakeColliderFromRecord(cr, meshAbsFor(fi), meshId, iscnDir, col))
                return false;
            if (primIdx > 0) ++dfs;   // "_colN" entity
            return true;
        };
        WalkNodeChildSlots(nr, volumeRecords.empty() ? nullptr : &volumeRecords,
                           collisionRecords.empty() ? nullptr : &collisionRecords,
                           slotVolume, slotMesh, slotCollider);

        auto cit = childrenOf.find(fr.idx);
        if (cit != childrenOf.end())
            for (auto it = cit->second.rbegin(); it != cit->second.rend(); ++it)
                stack.push_back({ *it, world, false });
    }
    return true;
}

// ==== The identity of a .meshlib that carries none ====
Resource::MeshIdentity SceneInstanceLoader::SynthesizeMeshLibIdentity(const std::string& meshlibPath,
                                                                    const uint8_t*     blob,
                                                                    size_t             size,
                                                                    const std::string& iscnPath)
{
    const std::string libCanon = Resource::MeshLibCanonicalPath(meshlibPath);

    std::string owner = iscnPath;
    if (owner.empty())
    {
        std::filesystem::path p = Sys::U8Path(meshlibPath);
        p.replace_extension(".iscn");
        owner = Sys::PathToUtf8(p);
    }

    std::vector<Resource::MeshGroupHint> groups;
    std::vector<std::string>             slotNames;

    // The owner is optional (a _collision.meshlib has none), so a missing or
    // foreign file is not an error: it only means "no grouping".
    std::vector<uint8_t> iscn;
    if (::Resource::AssetFS::Get().ReadFile(owner, iscn) && !iscn.empty() &&
        Resource::ValidateHeader(iscn.data(), iscn.size(), Resource::MAGIC_SCENE))
    {
        std::string text;
        {
            const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(iscn.data());
            const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(iscn.data()));
            const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(iscn.data()));
            size_t       n    = (std::min)(static_cast<size_t>(hdr->dataSize), iscn.size() - off);
            const void*  nul  = std::memchr(payl, 0, n);
            if (nul) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
            text.assign(payl, n);
        }

        const std::filesystem::path       iscnDir = Sys::U8Path(owner).parent_path();
        std::unordered_map<int, uint32_t> entryOfF;   // F idx -> entry in THIS library
        std::vector<NodeRecord>           nodes;
        std::istringstream                ss(text);
        std::string                       line;
        // An F-line's `sec=` names its entry before the `mesh=` hint does, as in the
        // loader. The keys this synthesis gives are positional (LegacySectionKey), so a
        // key maps back to its entry by trying them.
        std::unordered_map<uint64_t, uint32_t> legacyEntry;
        if (size >= sizeof(Resource::AssetHeader) + sizeof(Resource::MeshLibraryMetadata))
        {
            Resource::MeshLibraryMetadata meta{};
            std::memcpy(&meta, blob + sizeof(Resource::AssetHeader), sizeof(meta));
            for (uint32_t e = 0; e < meta.meshCount && e < (1u << 20); ++e)
                legacyEntry.emplace(Resource::LegacySectionKey(e), e);
        }
        while (std::getline(ss, line))
        {
            if (line.size() < 3 || line[1] != ' ') continue;
            if (line[0] == 'F')
            {
                FileRecord fr;
                if (!ParseFLine(line, fr) || fr.idx < 0) continue;
                // Only F-lines that name THIS library; a model may reference others.
                if (Resource::MeshLibCanonicalPath(Sys::PathToUtf8(iscnDir / Sys::U8Path(fr.file))) != libCanon)
                    continue;
                if (fr.sectionKey != 0)
                    if (const auto k = legacyEntry.find(fr.sectionKey); k != legacyEntry.end()) fr.subIdx = k->second;
                entryOfF.emplace(fr.idx, fr.subIdx);
                if (fr.subIdx >= slotNames.size()) slotNames.resize(size_t(fr.subIdx) + 1);
                std::string& slot = slotNames[fr.subIdx];
                if (slot.empty())
                    slot = !fr.name.empty()    ? fr.name
                         : fr.matFile.empty()  ? std::string()
                         : Sys::PathToUtf8(Sys::U8Path(fr.matFile).stem());
            }
            else if (line[0] == 'N')
            {
                NodeRecord nr;
                if (ParseNLine(line, nr)) nodes.push_back(std::move(nr));
            }
        }
        // Node order is the file's (pre-order) N-line order, so the grouping -
        // and with it which group owns an entry two nodes share - is the same
        // on every run, which is what makes this equal to the backfill.
        for (const NodeRecord& nr : nodes)
        {
            Resource::MeshGroupHint g;
            g.name = nr.name;
            for (int fi : nr.meshFileIndices)
            {
                auto it = entryOfF.find(fi);
                if (it != entryOfF.end()) g.sections.push_back(it->second);
            }
            if (!g.sections.empty()) groups.push_back(std::move(g));
        }
    }
    return Resource::SynthesizeMeshIdentity(libCanon, blob, size, groups, slotNames);
}


// ==== The model preload (see the header) ====
// The prepare half reads and decodes exactly what Load's first runtime spawn of
// the model would (the same helpers: PrepareModel, LoadIskel, the .meshlib entry
// decode, the .imorph block, PartKeyOf); the publish half does what that spawn's
// GPU / registry half would, one step at a time.
struct SceneInstanceLoader::ModelPreload
{
    struct Lib
    {
        std::string                      path;
        Resource::MeshLibrary::Prepared  prepared;
        Resource::Handle                 handle;
        bool                             done = false;
    };

    struct Part
    {
        int                                        fi = -1;
        SceneLoader::SkinnedMeshPending            pending;
        SkinnedMeshSubsystem::SharedSourceDerived  derived;   // on the worker
        std::shared_ptr<SkinnedMeshSubsystem::SourceBuild> build;   // in progress (main)
        bool                                       done = false;
    };

    std::string                      iscnPath, stamp;
    std::shared_ptr<ModelPrep>       prep;
    fs::path                         iscnDir;
    std::vector<std::string>         meshAbsPaths;   // per F-line
    std::vector<int>                 fiLib;          // per F-line: index into libs, -1 = none
    std::vector<uint32_t>            fiMeshId;       // per F-line

    std::vector<Lib>                 libs;
    // The skeleton - the template's registration (TemplateRegistrations).
    bool                             hasSkeleton = false;
    std::string                      iskelPath, tplKey;
    IskelData                        iskel;          // blend data moved into the parts
    std::vector<uint32_t>            skinnedMeshes;  // IskelMeshBlend::meshIndex, in .iskel order
    uint32_t                         skelIdx = kInvalidSkeletonIndex;
    bool                             skelDone = false;

    std::vector<Part>                parts;
    std::vector<std::string>         materialPaths;
    std::vector<std::unique_ptr<MaterialComponent>> materials;
    std::vector<uint8_t>             materialSkinned;
    bool                             prepOk = false, prepPublished = false;
    std::string                      error;
    // Whether a loader worker makes the GPU buffers (UploadModelPreload): chosen at
    // the first step from gfx.LoaderUploadsEnabled(); `uploaded` once that ran.
    bool                             loaderChosen = false, loader = false;
    bool                             buildsBegun = false, uploaded = false;
};

std::shared_ptr<SceneInstanceLoader::ModelPreload>
SceneInstanceLoader::PrepareModelPreload(const std::string& iscnPath, Resource::MeshLibrary& meshLib)
{
    PROF_SCOPE("SIL.PreloadPrepare");
    auto p = std::make_shared<ModelPreload>();
    p->iscnPath = iscnPath;
    p->stamp    = Resource::PrefabTemplate::FileStamp(iscnPath);
    auto prep = std::make_shared<ModelPrep>();
    if (!PrepareModel(iscnPath, *prep))
    {
        p->error = "the model does not read: " + iscnPath;
        return p;
    }
    p->prep    = prep;
    p->iscnDir = Sys::U8Path(iscnPath).parent_path();
    const std::vector<FileRecord>& fileRecords = prep->fileRecords;
    const size_t n = fileRecords.size();
    p->meshAbsPaths.resize(n);
    p->fiLib.assign(n, -1);
    p->fiMeshId.assign(n, 0u);

    // Each distinct library read and parsed once; each F-line's entry resolved
    // the way Load resolves it (the section key first, the mesh= hint else).
    for (size_t fi = 0; fi < n; ++fi)
    {
        const FileRecord& fr = fileRecords[fi];
        p->meshAbsPaths[fi]  = Sys::PathToUtf8(p->iscnDir / Sys::U8Path(fr.file));
        int li = -1;
        for (size_t k = 0; k < p->libs.size(); ++k)
            if (p->libs[k].path == p->meshAbsPaths[fi]) { li = static_cast<int>(k); break; }
        if (li < 0)
        {
            ModelPreload::Lib lib;
            lib.path = p->meshAbsPaths[fi];
            (void)meshLib.PrepareFromFile(lib.path, lib.prepared);   // a failure logs as Load's would
            p->libs.push_back(std::move(lib));
            li = static_cast<int>(p->libs.size() - 1);
        }
        const Resource::MeshLibrary::Prepared& lp = p->libs[li].prepared;
        if (!lp.ok) continue;
        p->fiLib[fi] = li;
        uint32_t entryId = fr.subIdx;
        if (fr.sectionKey != 0 && lp.identity)
        {
            const int byKey = lp.identity->FindSection(fr.sectionKey);
            if (byKey >= 0) entryId = static_cast<uint32_t>(byKey);
        }
        p->fiMeshId[fi] = entryId;
    }

    // The file stamps the part keys mix, once each (as a BatchScope asks).
    std::unordered_map<std::string, std::string> stamps;
    auto stampOf = [&stamps](const std::string& path) -> const std::string&
    {
        auto it = stamps.find(path);
        if (it == stamps.end()) it = stamps.emplace(path, Resource::PrefabTemplate::FileStamp(path)).first;
        return it->second;
    };

    // The skeleton and the skinned parts (Load's needSkelMap branch).
    std::vector<uint8_t> fiSkinned(n, 0);
    if (!prep->skelRelFile.empty())
    {
        p->hasSkeleton = true;
        p->iskelPath   = Sys::PathToUtf8(p->iscnDir / Sys::U8Path(prep->skelRelFile));
        const uint64_t    vbSig    = Resource::VirtualBonesSignature(prep->modelVirtualBones.bones);
        const std::string shareKey = vbSig ? p->iskelPath + "#vb" + Resource::MeshKeyToHex(vbSig) : p->iskelPath;
        if (!stampOf(p->iskelPath).empty()) p->tplKey = shareKey + "@" + stampOf(p->iskelPath);
        p->iskel = LoadIskel(p->iskelPath);
        if (p->iskel.valid && !p->iskel.meshBlends.empty() && p->iskel.skeleton)
        {
            // The virtual bones after the real ones, as Load registers it.
            if (!prep->modelVirtualBones.bones.empty())
            {
                std::vector<std::string> warns;
                (void)Resource::ApplyVirtualBones(*p->iskel.skeleton, prep->modelVirtualBones.bones, &warns);
                for (const std::string& w : warns)
                    LOG_WARNING("SceneInstanceLoader: '%s' %s", iscnPath.c_str(), w.c_str());
            }
            p->iskel.skeleton->virtualBoneSignature = vbSig;
        }

        std::unordered_map<std::string, std::vector<uint8_t>> imorphBlobs;
        for (IskelMeshBlend& mb : p->iskel.meshBlends)
        {
            p->skinnedMeshes.push_back(mb.meshIndex);
            const int fi = static_cast<int>(mb.meshIndex);
            if (fi < 0 || fi >= static_cast<int>(n) || p->fiLib[fi] < 0) continue;
            fiSkinned[fi] = 1;
            const Resource::MeshLibrary::Prepared& lp = p->libs[p->fiLib[fi]].prepared;
            // DecomposeMeshSource's dispatch, from the blob already read.
            ImshStreams streams;
            {
                PROF_SCOPE("SIL.DecodeMesh");
                std::string ext = p->meshAbsPaths[fi].substr(p->meshAbsPaths[fi].find_last_of('.') + 1);
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                streams = (ext == "meshlib") ? DecomposeMeshLibEntryFromBlob(lp.blob, p->fiMeshId[fi])
                                             : DecomposeImsh(p->meshAbsPaths[fi]);
            }
            if (!streams.valid) continue;
            const uint32_t verts = static_cast<uint32_t>(streams.positions.size());
            // A part whose blend data does not fit is left to the spawn, which
            // decodes it and reports the mismatch.
            if (mb.blendData.size() != verts) continue;

            ModelPreload::Part part;
            part.fi = fi;
            SceneLoader::SkinnedMeshPending& pd = part.pending;
            pd.restPositions = std::move(streams.positions);
            pd.restNormals   = std::move(streams.normals);
            pd.uvs           = std::move(streams.uvs);
            pd.uvs1          = std::move(streams.uvs1);
            pd.uvs2          = std::move(streams.uvs2);
            pd.uvs3          = std::move(streams.uvs3);
            pd.tangents      = std::move(streams.tangents);
            pd.indices       = std::move(streams.indices);
            pd.blendData     = std::move(mb.blendData);

            // The part's .imorph block (Load's morph section, without its cache).
            for (const auto& mref : prep->morphFileRefs)
            {
                if (mref.meshFileIdx != fi) continue;
                const std::string imorphPath = Sys::PathToUtf8(p->iscnDir / Sys::U8Path(mref.file));
                auto cit = imorphBlobs.find(imorphPath);
                if (cit == imorphBlobs.end())
                {
                    std::vector<uint8_t> blob;
                    ::Resource::AssetFS::Get().ReadFile(imorphPath, blob);
                    cit = imorphBlobs.emplace(imorphPath, std::move(blob)).first;
                }
                Resource::MorphLibrary::File mfile;
                std::string                  mwhy;
                if (cit->second.empty() || !Resource::MorphLibrary::Read(cit->second.data(), cit->second.size(), mfile, &mwhy))
                    break;
                uint64_t sectionKey = fileRecords[fi].sectionKey;
                if (lp.identity && p->fiMeshId[fi] < lp.identity->sections.size())
                    sectionKey = lp.identity->sections[p->fiMeshId[fi]].key;
                const Resource::MorphLibrary::Block* blk = mfile.Find(static_cast<uint32_t>(fi), sectionKey);
                if (!blk || blk->vertexCount != verts) break;
                const size_t vc = blk->vertexCount;
                const size_t m  = static_cast<size_t>(blk->count) * vc;
                pd.morphCount      = blk->count;
                pd.morphHasNormals = blk->hasNormals;
                pd.morphDeltas.resize(blk->hasNormals ? 2 * m : m);
                pd.morphNames.resize(blk->count);
                pd.morphSlotOfLocal.resize(blk->count);
                DirectX::XMFLOAT3* dst = pd.morphDeltas.data();
                for (uint32_t t = 0; t < blk->count; ++t)
                {
                    pd.morphNames[t]       = blk->Name(t);
                    pd.morphSlotOfLocal[t] = blk->Slot(t);
                    blk->CopyPositions(t, dst + t * vc);
                    if (blk->hasNormals) blk->CopyNormals(t, dst + m + t * vc);
                }
                break;
            }
            pd.sourceKey = PartKeyOf(p->meshAbsPaths[fi], p->fiMeshId[fi], p->iskelPath, fi,
                                     prep->morphFileRefs, p->iscnDir, stampOf);
            // The source's per-vertex results here too: the main step then only
            // creates buffers. The skeleton as it will be registered.
            if (p->iskel.skeleton)
                SkinnedMeshSubsystem::DeriveSharedSource(pd, p->iskel.skeleton->boneCount,
                                                         p->iskel.skeleton->morphTargetCount, part.derived);
            p->parts.push_back(std::move(part));
        }
    }

    // Each F-line's material, as SpawnSingleNode names and loads it; the process
    // cache keeps what this parses.
    for (size_t fi = 0; fi < n; ++fi)
    {
        if (fileRecords[fi].matFile.empty()) continue;
        const std::string matPath = Sys::RelativizeAssetPath(Sys::PathToUtf8(
            (p->iscnDir / Sys::U8Path(fileRecords[fi].matFile)).lexically_normal()));
        size_t k = 0;
        for (; k < p->materialPaths.size(); ++k) if (p->materialPaths[k] == matPath) break;
        if (k == p->materialPaths.size())
        {
            p->materialPaths.push_back(matPath);
            auto mc = std::make_unique<MaterialComponent>();
            if (!Resource::LoadMaterial(matPath, *mc)) mc.reset();
            p->materials.push_back(std::move(mc));
            p->materialSkinned.push_back(0);
        }
        if (fiSkinned[fi]) p->materialSkinned[k] = 1;
    }
    p->prepOk = true;
    return p;
}

bool SceneInstanceLoader::ModelPreloadNeedsUpload(const ModelPreload& p)
{
    return p.prepOk && p.loader && p.buildsBegun && !p.uploaded;
}

void SceneInstanceLoader::UploadModelPreload(ModelPreload& p, IGraphicsDevice& gfx)
{
    PROF_SCOPE("SIL.PreloadUpload");
    if (!p.prepOk || p.uploaded) return;
    {
        // (On when the mode was chosen; a Begin that refuses leaves the main steps.)
        LoaderUploadScope scope(gfx);
        if (!scope.Active()) { p.loader = false; p.uploaded = true; return; }
        for (ModelPreload::Lib& lib : p.libs)
            if (!lib.done && lib.prepared.ok) (void)Resource::MeshLibrary::CreateCommitBuffers(lib.prepared, gfx);
        for (ModelPreload::Part& part : p.parts)
            if (!part.done && part.build) (void)Renderer::BuildSharedSkinnedSourceOnLoader(*part.build);
    }   // the copies done, on this worker
    p.uploaded = true;
}

bool SceneInstanceLoader::PublishModelPreloadStep(ModelPreload& p, IGraphicsDevice& gfx,
                                                  Resource::MeshLibrary& meshLib, Renderer& renderer)
{
    if (!p.prepOk) return false;
    if (!p.loaderChosen)
    {
        p.loaderChosen = true;
        p.loader       = gfx.LoaderUploadsEnabled();
    }
    // The records into the spawn template first (no GPU: with the next step).
    if (!p.prepPublished)
    {
        p.prepPublished = true;
        if (!p.stamp.empty()) SpawnPreps()[p.iscnPath] = CachedSpawnPrep{ p.stamp, p.prep };
    }
    // With loader uploads the GPU buffers go to a loader worker: the skeleton and
    // the builds first (they need its index), then UploadModelPreload, then the
    // publishes.
    if (p.loader && !p.uploaded)
    {
        if (p.hasSkeleton && !p.skelDone) { /* the skeleton step below */ }
        else if (!p.buildsBegun)
        {
            PROF_SCOPE("SIL.PreloadBeginBuilds");
            for (ModelPreload::Part& part : p.parts)
            {
                if (part.done || part.build) continue;
                if (p.skelIdx == kInvalidSkeletonIndex || renderer.HasSharedSkinnedSource(part.pending.sourceKey))
                {
                    part.done    = true;
                    part.pending = SceneLoader::SkinnedMeshPending{};
                    continue;
                }
                part.pending.skeletonIndex = p.skelIdx;
                part.build = renderer.BeginSharedSkinnedSourceBuild(part.pending, &part.derived);
                if (!part.build)
                {
                    LOG_WARNING("SceneInstanceLoader: preload of '%s' could not build part meshFileIdx=%d - the spawn "
                                "decodes it", p.iscnPath.c_str(), part.fi);
                    part.done = true;
                }
            }
            p.buildsBegun = true;
            return false;   // ModelPreloadNeedsUpload: the worker's turn
        }
        else
            return false;   // waiting for the upload
    }
    if (p.loader && p.uploaded)
    {
        // The publishes: a library slot a step, then every part's cache insert.
        for (ModelPreload::Lib& lib : p.libs)
        {
            if (lib.done) continue;
            if (!lib.prepared.ok) { lib.done = true; continue; }
            PROF_SCOPE("SIL.PreloadPublishLib");
            while (meshLib.CommitStep(lib.prepared, gfx, lib.handle)) {}
            lib.done     = true;
            lib.prepared = Resource::MeshLibrary::Prepared{};   // the blob: on the GPU now
            return true;
        }
        bool any = false;
        for (ModelPreload::Part& part : p.parts)
        {
            if (part.done) continue;
            if (part.build) (void)renderer.PublishSharedSkinnedSourceBuild(*part.build);
            part.build.reset();
            part.done    = true;
            part.pending = SceneLoader::SkinnedMeshPending{};   // the streams: on the GPU now
            part.derived = SkinnedMeshSubsystem::SharedSourceDerived{};
            any = true;
        }
        return any;
    }
    // A library: one of its buffers a step (MeshLibrary::CommitStep).
    for (ModelPreload::Lib& lib : p.libs)
    {
        if (p.loader) break;   // a loader worker makes them (above)
        if (lib.done) continue;
        if (!lib.prepared.ok) { lib.done = true; continue; }
        PROF_SCOPE("SIL.PreloadMeshLib");
        if (meshLib.CommitStep(lib.prepared, gfx, lib.handle)) return true;
        lib.done     = true;
        lib.prepared = Resource::MeshLibrary::Prepared{};   // the blob: decoded already
        return true;
    }
    // The skeleton, as the template's registration (Load's tplKey branch).
    if (p.hasSkeleton && !p.skelDone)
    {
        p.skelDone = true;
        if (p.tplKey.empty() || !p.iskel.valid || !p.iskel.skeleton || p.skinnedMeshes.empty())
            return true;   // nothing a spawn could reuse: it registers on its own
        PROF_SCOPE("SIL.PreloadSkeleton");
        auto& regs = TemplateRegistrations();
        const auto it = regs.find(p.tplKey);
        if (it != regs.end() && it->second.skelIdx < renderer.GetSkeletonRegistry().Count())
        {
            p.skelIdx = it->second.skelIdx;
            return true;
        }
        p.skelIdx = renderer.GetSkeletonRegistry().Register(std::move(*p.iskel.skeleton));
        p.iskel.skeleton.reset();
        const size_t clipCount = p.iskel.clips.size();
        for (auto& clip : p.iskel.clips)
            renderer.GetClipLibrary().Register(std::move(clip));
        p.iskel.clips.clear();
        TemplateRegistration& tr = regs[p.tplKey];
        tr.skelIdx       = p.skelIdx;
        tr.clipCount     = clipCount;
        tr.skinnedMeshes = p.skinnedMeshes;
        ++Resource::PrefabTemplate::Stats().skeletonRegistrations;
        LOG_INFO("SceneInstanceLoader: preload registered skeleton idx=%u for '%s' (the runtime spawn template; "
                 "registry now %u skeletons)", p.skelIdx, p.iscnPath.c_str(), renderer.GetSkeletonRegistry().Count());
        return true;
    }
    // A skinned part's shared source: one stage (a few buffers) a step.
    for (ModelPreload::Part& part : p.parts)
    {
        if (p.loader) break;   // a loader worker makes them (above)
        if (part.done) continue;
        PROF_SCOPE("SIL.PreloadSkinPart");
        if (!part.build)
        {
            if (p.skelIdx == kInvalidSkeletonIndex || renderer.HasSharedSkinnedSource(part.pending.sourceKey))
            {
                part.done    = true;
                part.pending = SceneLoader::SkinnedMeshPending{};
                continue;
            }
            part.pending.skeletonIndex = p.skelIdx;
            part.build = renderer.BeginSharedSkinnedSourceBuild(part.pending, &part.derived);
            if (!part.build)
            {
                LOG_WARNING("SceneInstanceLoader: preload of '%s' could not build part meshFileIdx=%d - the spawn "
                            "decodes it", p.iscnPath.c_str(), part.fi);
                part.done = true;
                continue;
            }
        }
        if (renderer.StepSharedSkinnedSourceBuild(*part.build)) return true;
        part.build.reset();
        part.done    = true;
        part.pending = SceneLoader::SkinnedMeshPending{};   // the streams: on the GPU now
        part.derived = SkinnedMeshSubsystem::SharedSourceDerived{};
        return true;
    }
    return false;
}

SceneInstanceLoader::ModelPreloadInfo SceneInstanceLoader::DescribeModelPreload(const ModelPreload& p)
{
    ModelPreloadInfo info;
    info.ok    = p.prepOk;
    info.error = p.error;
    info.stepsTotal = static_cast<uint32_t>(1 + p.libs.size() + (p.hasSkeleton ? 1 : 0) + p.parts.size());
    info.stepsDone  = p.prepPublished ? 1u : 0u;
    for (const auto& lib : p.libs)   if (lib.done)  ++info.stepsDone;
    if (p.hasSkeleton && p.skelDone) ++info.stepsDone;
    for (const auto& part : p.parts) if (part.done) ++info.stepsDone;
    if (!p.prepPublished)                                          info.next = "records";
    else if (std::any_of(p.libs.begin(), p.libs.end(), [](const auto& l) { return !l.done; }))
                                                                   info.next = "meshlib";
    else if (p.hasSkeleton && !p.skelDone)                         info.next = "skeleton";
    else if (std::any_of(p.parts.begin(), p.parts.end(), [](const auto& q) { return !q.done; }))
                                                                   info.next = "skinnedPart";
    info.materialPaths   = p.materialPaths;
    info.materialSkinned = p.materialSkinned;
    for (const auto& mc : p.materials) info.materials.push_back(mc.get());
    return info;
}

void SceneInstanceLoader::ReleaseModelPreload(ModelPreload& p, Resource::MeshLibrary& meshLib, IGraphicsDevice& gfx)
{
    for (ModelPreload::Lib& lib : p.libs)
    {
        meshLib.AbandonCommit(lib.prepared, gfx);   // a commit given up midway
        if (lib.handle.IsValid()) meshLib.Release(lib.handle, gfx);
        lib.handle = Resource::Handle{};
    }
    // A source given up midway frees what it made (SourceBuild's destructor).
    for (ModelPreload::Part& part : p.parts) part.build.reset();
}
