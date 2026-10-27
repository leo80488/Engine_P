#pragma once

// SceneInstanceLoader: instantiates a cooked .iscn model into the ECS world. Reads the .iscn
// text, loads the geometry its F-lines name (.meshlib entries through MeshLibrary, or per-mesh
// .imsh files) and builds a Parent / Children hierarchy with a MaterialComponent on every mesh
// entity. With a K line (skeleton file) and a renderer, the skeleton is deserialized and the
// skinned mesh entities are registered through Renderer::RegisterSkinnedMeshFull().
// Also the model file's readers and writers: material bindings, keys, asset collision,
// sockets, virtual bones, placements and file references.

#include "ECS/ECS.h"
#include "ECS/PhysicsComponents.h"   // ColliderComponent (PartColliderDefault)
#include "Resource/MeshLibIdentity.h"
#include "Resource/ModelCollision.h"  // MeshBodySetup (ReplaceModelCollision)
#include "Resource/ModelShapeKeys.h"  // MeshShapeKeySetup (ReplaceModelShapeKeys)
#include "Resource/ModelSockets.h"    // ModelSocket(s) (DeriveModelSockets)
#include "Resource/ModelVirtualBones.h" // ModelVirtualBone (ReplaceModelVirtualBones)
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource { class AssetManager; class MeshLibrary; }
struct MaterialComponent;
class Renderer;
class IGraphicsDevice;
struct SkeletonAsset;
class SkeletonRegistry;
struct ClipAsset;
struct BlendVertex;

class SceneInstanceLoader
{
public:
    // RAII memoisation scope for a batch of Load() calls.
    //
    // While at least one BatchScope is alive, Load() memoises the decoded `.meshlib` / `.imorph`
    // payloads keyed by (path, meshId) and reuses them across calls, so spawning one character N
    // times decodes it once (~50 ms per skinned copy otherwise). The cache is released when the
    // outermost scope ends, so it never serves stale data after a re-import: open a scope around
    // a load batch, never for the lifetime of the process.
    //
    // Pass @p gfx (AssetManager::GetGraphicsDevice()) to also hold one buffer-upload / buffer-heap
    // batch open across the whole batch: Load() opens its own upload batch per call, and
    // EndBufferUploadBatch flushes and waits when the depth returns to 0; held open here, the
    // per-Load scopes nest and only this one flushes. Staging self-limits mid-batch
    // (FlushBatchStagingIfNeeded at 64 MB / 512 buffers), so holding it open does not grow
    // without bound. Pass nullptr for memoisation only.
    //
    // Nestable (flat depth counter). Main thread only, like Load() itself.
    class BatchScope
    {
    public:
        explicit BatchScope(IGraphicsDevice* gfx = nullptr);
        ~BatchScope();
        BatchScope(const BatchScope&)            = delete;
        BatchScope& operator=(const BatchScope&) = delete;

    private:
        IGraphicsDevice* m_gfx = nullptr;   // non-null only on the outermost scope
    };

    // RAII: while one is alive, Load() reuses a skeleton registration an earlier load under such a
    // scope made for the same model skeleton - the .iskel path, its virtual-bone records and the
    // .iskel file's size + write time - instead of registering another SkeletonAsset (~650 KB,
    // append-only registry) and its embedded clips. For spawns that repeat all session, such as an
    // editor panel's preview stage (every tab switch, host switch and attachment edit respawns
    // it). Loads outside a scope never see or fill that map: a placed model keeps its own
    // registration. A changed .iskel (a re-import, a humanoid Save) is a new key. Nestable; main
    // thread only, like Load().
    class RegistrationReuseScope
    {
    public:
        RegistrationReuseScope();
        ~RegistrationReuseScope();
        RegistrationReuseScope(const RegistrationReuseScope&)            = delete;
        RegistrationReuseScope& operator=(const RegistrationReuseScope&) = delete;
    };

    struct LoadResult
    {
        Entity   rootEntity      { NullEntity };
        uint32_t nodeCount       { 0 };
        uint32_t meshEntityCount { 0 };
        bool     success         { false };
    };

    // ---- A runtime spawn's model, preloaded ----
    struct ModelPreload;

    struct ModelPreloadInfo
    {
        bool        ok = false;                  // prepared (the steps may still be running)
        std::string error;                       // why not
        uint32_t    stepsDone = 0, stepsTotal = 0;
        const char* next = "";                   // the next step's kind ("" = none)
        // The F-lines' materials (canonical paths, unique) as loaded, null where
        // the load failed; skinned = some skinned part draws with it. Valid
        // while the preload lives.
        std::vector<std::string>              materialPaths;
        std::vector<const MaterialComponent*> materials;
        std::vector<uint8_t>                  materialSkinned;
    };

    // One skinned mesh's per-vertex influences as the .iskel stores them (its blend stream;
    // meshIndex = the model's mesh entry it skins).
    struct SkinBlendStream
    {
        uint32_t                 meshIndex = 0;
        std::vector<BlendVertex> weights;
    };

    struct PreviewMeshData {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t> indices;
        std::vector<BlendVertex> weights;
    };

    // ---- .iscn material bindings (read and write-back) ----
    // An F-line's `mat=` is the model's default material for that mesh entry, and the only place
    // that binding is recorded: nothing else writes a .iscn from the live world, and SaveScene
    // skips every .iscn-spawned entity (they respawn from this file).
    // Keyed by `mesh=` (the MeshLibraryEntry index), not by the F index: meshId is what a spawned
    // entity carries (MeshLibRef::meshId), and the F index cannot be recovered from it. For
    // importers that emit one F-line per mesh entry (PMX, assimp) the two are identical. Unreal
    // level imports point several F-lines at one entry with different materials; such an entry
    // is ambiguous: ReadMaterialBindings flags it and WriteMaterialBindings skips it (with a
    // warning) rather than guess.
    struct MaterialBinding
    {
        // The mesh-asset path a spawned entity carries in MeshSourcePath:
        // Sys::PathToUtf8(iscnDir / F-line file=), native separators. With meshId this is the exact
        // key SaveScene's MC/FS/MO/MR override lines use, so a caller can byte-compare without
        // re-deriving it.
        std::string meshAbsPath;
        uint32_t    meshId  { 0 };
        std::string matPath;        // project-root-relative .imat; "" = none
        // The F-line's `name=` (the authoring tool's material name), percent-decoded; empty when the
        // line has none. It is the only key that survives a re-import which adds, removes or reorders
        // materials (meshId does not), so a merge across imports must match on it and fall back to
        // meshId.
        std::string name;
        bool        ambiguous{ false };  // >1 F-line names this entry, with different materials
        // The F-line's `sec=` (the entry's SectionKey); 0 when the line has none. A caller holding
        // the library resolves the entry by it (MeshLibrary::FindEntryBySectionKey); `meshId` is then
        // only the hint the key overrides.
        uint64_t    sectionKey{ 0 };
    };

    struct DefaultMaterialEdit
    {
        std::string meshAbsPath;   // MaterialBinding::meshAbsPath's spelling
        uint32_t    meshId = 0;
        std::string material;      // project-relative; "" = none
        // The section's key. Non-zero = F-lines are matched by their `sec=` (a stale `mesh=` hint
        // cannot misdirect the write); 0 = by meshAbsPath + meshId.
        uint64_t    sectionKey = 0;
    };

    // ---- Keys in the model file ----
    // The identity a library has for keys: the file's IDNT, else the loader's synthesis (from its
    // own model file) - exactly MeshLibrary's identity provider. Read once per path for a
    // stamping pass.
    class LibraryIdentityCache
    {
    public:
        // nullptr when the library cannot be read.
        const Resource::MeshIdentity* Get(const std::string& meshlibPath);
    private:
        std::unordered_map<std::string, std::unique_ptr<Resource::MeshIdentity>> m_ids;
    };

    struct ModelKeyReport
    {
        uint32_t fLines = 0;       // F-lines naming a .meshlib entry
        uint32_t fAdded = 0;       // got a `sec=`
        uint32_t fKept = 0;        // already keyed, hint agrees
        uint32_t fHintFixed = 0;   // keyed; `mesh=` was stale and now names the key's entry
        uint32_t fDangling = 0;    // keyed with a key the library does not have (left alone)
        uint32_t fUnresolved = 0;  // the library does not read / the entry is out of range
        uint32_t fLegacy = 0;      // F-lines naming a per-mesh file (.imsh): no identity, never keyed
        // N-lines with mesh refs: nLines = nAdded + nKept + nChanged + nMixed + nLegacy.
        // nMixed = the refs are not sections of ONE Mesh (or one did not resolve);
        // nLegacy = every ref names a per-mesh file.
        uint32_t nLines = 0, nAdded = 0, nKept = 0, nChanged = 0, nMixed = 0, nLegacy = 0;
        uint32_t meLines = 0;      // ME records written
        bool     changed = false;  // the text differs from the file's
        bool     written = false;
    };

    // ---- C records -> the model's asset collision ----
    struct CollisionMigrationReport
    {
        uint32_t cLines        = 0;   // C records in the file
        uint32_t migrated      = 0;   // C records converted (removed)
        uint32_t meshes        = 0;   // Meshes given a BO record
        uint32_t complexMeshes = 0;   //   ... of them complexity=complex
        uint32_t bodyPrims     = 0;   // PR records of body prims written
        uint32_t sectionPrims  = 0;   // PR type=mesh records of own sections written
        uint32_t compounds     = 0;   // Meshes whose body is several prims
        uint32_t colEntities   = 0;   // `_colN` entities the spawn stops making (all placements)
        uint32_t boLines       = 0;   // BO records already in the file
        std::vector<std::pair<uint64_t, std::string>> refused;   // Mesh key, why its C records stay
        bool     changed = false;
        bool     written = false;
    };

    struct PartColliderDefault
    {
        std::string       lib;
        uint32_t          entry       = 0;
        bool              hasCollider = false;
        ColliderComponent collider;
    };

    // ---- Read-only placement view (asset preview) ----
    // One part Load() would spawn, as data (a row of ReadMeshPlacements).
    struct MeshPlacement
    {
        // The F-line's `file=` resolved against the .iscn's directory and project-relativised: the
        // same expression ReadMaterialBindings uses for meshAbsPath, so the two compare equal.
        std::string meshPath;
        uint32_t    meshId = 0;
        // Project-relative .imat / .imatinst; empty when the F-line has no
        // `mat=`, which the caller reads as "the engine default material".
        std::string matPath;
        // The owning N-node's world transform, composed through its parent chain; row-major, ready
        // for a draw packet. The root node's own local transform is deliberately not in it:
        // PrefabSerializer::LoadPrefab replaces the spawned .iscn root's LocalTransform with the
        // prefab N-line's, so composing both would apply the root's placement twice. Multiply by the
        // owning prefab node's world (what LoadPrefab effectively does) and nothing else.
        float       world[16] = { 1.f, 0.f, 0.f, 0.f,
                                  0.f, 1.f, 0.f, 0.f,
                                  0.f, 0.f, 1.f, 0.f,
                                  0.f, 0.f, 0.f, 1.f };
        // Where this part lands in the spawned subtree, counted as CollectSubtreeDFS counts: the .iscn
        // root is 0, then that node's own child entities in the one canonical order SpawnSingleNode
        // creates them (its `_volN` volume entities, then each accepted mesh ref followed by that
        // ref's extra `_colN` compound-collider entities), then its child nodes' subtrees in
        // ascending node order. Do not re-implement that order: ReadMeshPlacements and
        // SpawnSingleNode both walk WalkNodeChildSlots (SceneInstanceLoader.cpp); a second copy
        // drifts and sends a prefab's overrides to the wrong parts.
        // PrefabSerializer::LoadPrefab applies a prefab's saved per-part component blocks by this key
        // (entityMap[sceneIdx + i] = loadedEntities[i]), so reproducing a prefab's material overrides
        // must match on it alone. It is a position, not an identity: a re-import that reorders meshes
        // moves every override, which LoadPrefab only warns about.
        uint32_t    dfsIndex = 0;

        // The F-line's `sec=` (0 = none): a reader that loads the library resolves the entry by it;
        // meshId is the hint.
        uint64_t    sectionKey = 0;

        // The F-line's index: what an .iskel blend stream names (its meshIndex), so a reader pairs a
        // skinned part's vertices with their weights the way Load registers it.
        uint32_t    fileIndex = 0;
    };

    // ---- Every file a .iscn names (the asset catalog) ----
    struct FileReference
    {
        // kind: 'F' mesh file or material, 'K' skeleton, 'M' morph, 'C' / 'V' / 'P' collision
        // library (C, V, PR records).
        char        kind = 'F';
        std::string key;            // "file" / "mat" / "lib"
        std::string path;           // project-relative, '/' separators
        unsigned    line = 0;       // 1-based
    };

    // ---- Load into a World ----
    // Loads @p iscnPath into @p world: each .meshlib through @p meshLib, one entity per mesh ref
    // with a MeshLibRef to the library's entry; @p assetMgr resolves the texture / material (.imat)
    // paths. A non-null @p renderer enables skeleton / animation loading when the .iscn names a
    // .iskel. Main thread only.
    static LoadResult Load(const std::string&      iscnPath,
                           World&                  world,
                           Resource::AssetManager& assetMgr,
                           Resource::MeshLibrary&  meshLib,
                           Renderer*               renderer = nullptr);

    // ---- A runtime spawn's model, preloaded ----
    // Does what the first runtime spawn of a model would read, decode, register and upload, ahead
    // of it and in two halves, so that spawn finds the model's records in the spawn template
    // (Resource/PrefabTemplate.h), its libraries loaded, the template's skeleton registered and
    // every skinned part's shared source live: it reads and decodes nothing (the warm path, with
    // the same keys).
    //
    // PrepareModelPreload - any thread (a TaskSystem worker): the records, each .meshlib read and
    //   parsed (MeshLibrary::PrepareFromFile), the .iskel (virtual bones applied), every skinned
    //   part's streams, morph block and shared-source key, each F-line's material
    //   (Resource::LoadMaterial, whose process cache the spawn then hits). No World, registry or GPU.
    // PublishModelPreloadStep - main thread, game half, render lane idle (App's AssetTick): one
    //   step of the rest - the records into the template, a library committed, the skeleton
    //   registered as the template's, one part's shared source built
    //   (SkinnedMeshSubsystem::PublishSharedSource). Returns false once nothing is left or, with
    //   the device's loader uploads on, once only the GPU buffers are left:
    //   ModelPreloadNeedsUpload then says so.
    // UploadModelPreload - any thread (a TaskSystem worker): makes every library's buffers and
    //   every part's shared source inside the device's loader upload scope; the steps after it
    //   only publish them (the library slots, the cache inserts).
    static std::shared_ptr<ModelPreload> PrepareModelPreload(const std::string& iscnPath,
                                                             Resource::MeshLibrary& meshLib);
    static bool PublishModelPreloadStep(ModelPreload& p, IGraphicsDevice& gfx,
                                        Resource::MeshLibrary& meshLib, Renderer& renderer);
    static bool ModelPreloadNeedsUpload(const ModelPreload& p);
    static void UploadModelPreload(ModelPreload& p, IGraphicsDevice& gfx);

    static ModelPreloadInfo DescribeModelPreload(const ModelPreload& p);
    // Main thread: drops the library references the preload holds.
    static void ReleaseModelPreload(ModelPreload& p, Resource::MeshLibrary& meshLib, IGraphicsDevice& gfx);

    // ---- Skeleton and preview reads ----
    // Loads a standalone .iskel into a SkeletonAsset (with its humanoid profile, if present) plus
    // its embedded animation clips, for tools such as the retarget baker that need a source
    // skeleton without spawning a scene. Returns false if the file is missing or invalid.
    static bool LoadSkeletonFile(const std::string&      iskelPath,
                                 SkeletonAsset&          outSkeleton,
                                 std::vector<ClipAsset>& outClips);

    // The skeleton a model names: its K line's .iskel, project-relative ("" = none, or not a
    // model). Reads the file through AssetFS.
    static std::string ModelSkeletonPath(const std::string& iscnPath);

    // LoadSkeletonFile plus the file's blend streams when @p outBlends is not null; read-only, no
    // meshlib needed (Resource::InspectSkeleton's weight source).
    static bool LoadSkeletonFile(const std::string&            iskelPath,
                                 SkeletonAsset&                outSkeleton,
                                 std::vector<ClipAsset>&       outClips,
                                 std::vector<SkinBlendStream>* outBlends);

    // Read-only CPU preview data; no entities, physics or GPU resources created.
    static bool LoadPreviewMeshes(const std::string& meshPath,const std::string& skeletonPath,
                                  SkeletonAsset& skeleton,std::vector<PreviewMeshData>& meshes);

    // ---- .iscn material bindings (read and write-back) ----
    // Parses just the F-lines of a .iscn and returns each mesh entry's material path, resolved as
    // Load() resolves it (relative to the .iscn's own directory, then project-relativised).
    // Read-only, through AssetFS, so it works on a packed build.
    static bool ReadMaterialBindings(const std::string&            iscnPath,
                                     std::vector<MaterialBinding>& out);

    // Rewrites the `mat=` of every F-line whose `mesh=` appears in @p bindings, in place, keeping
    // the rest of the file byte for byte.
    //
    // This edits a shared source asset: the new material becomes the default for every scene and
    // instance that spawns the model. Call it only from an explicit user action, never as a side
    // effect of saving a scene (that is what the .iscene MR line is for).
    //
    // Skips (with a warning) a binding whose entry the file names on no F-line or on more than
    // one - counted from the file, not taken from `ambiguous` - and writes the rest. Writes
    // through the writable filesystem, not AssetFS, so a pak-only .iscn is rejected.
    // @p outChanged receives the number of F-lines changed.
    static bool WriteMaterialBindings(const std::string&                  iscnPath,
                                      const std::vector<MaterialBinding>& bindings,
                                      int*                                outChanged = nullptr);

    // Makes @p material the default material of one library entry of this model (the Mesh Editor's
    // Save). The default is what MeshAssetRegistry reads: the first F-line naming the entry. Every
    // F-line naming it with that same material is rewritten (the default placed again - four
    // wheels of one car); a line carrying another material is a placement override and keeps it.
    // All or nothing: an entry the model has no F-line for (its default comes from the library's
    // own model file) refuses the whole call and writes nothing. Written through a temporary and
    // a rename. @p outChanged = F-lines rewritten.
    static bool WriteDefaultMaterials(const std::string&                     iscnPath,
                                      const std::vector<DefaultMaterialEdit>& edits,
                                      int*                                   outChanged = nullptr,
                                      std::string*                           outError   = nullptr);

    // ---- Keys in the model file ----
    // A .iscn names each library entry by its position (`F ... mesh=<i>`), which a re-import that
    // reorders the source moves. So every F-line also carries its entry's SectionKey
    // (`sec=<hex16>`, after `mesh=`, before `name=` / `mat=`), every N-line that places one Mesh
    // carries that Mesh's key (`mkey=<hex16>`, before `name=`), and one
    // `ME key=<hex16> name=<pct>` line per placed Mesh spares text tools the binary. The loader
    // follows `sec=`; `mesh=` stays as the hint an older binary reads (its parser ends a value at
    // the next key it knows, so `mesh=3 sec=...` still reads 3, and it skips two-letter records).
    //
    // Stamps @p iscnPath's keys: F `sec=` from each entry's identity (and a stale `mesh=` hint
    // refreshed from a line's existing key), N `mkey=` for a node whose refs are all sections of
    // one Mesh, the `ME` lines regenerated. Idempotent. Written (temp + rename) only when
    // @p apply and something changed. `sub=` / .imsh lines are left as they are.
    static bool StampModelKeys(const std::string& iscnPath, bool apply, ModelKeyReport& report,
                               LibraryIdentityCache& libraries, std::string* outError = nullptr);

    // ---- C records -> the model's asset collision ----
    // A C record (`C f=<F> shape=...`) gives collision per F-line: the first primitive rides on
    // that part, each further one becomes a `_colN` sibling entity of the node. BO / PR records
    // (Resource/ModelCollision.h) give it once per Mesh. MigrateModelCollision converts Mesh by
    // Mesh, and only where the records spawn the same collision on every placement:
    //   * body prims - a hull / mesh naming a library (`lib=`), a hull of the part's own section,
    //     a box / sphere / capsule: every node placing the Mesh carries the same list on the Mesh's
    //     first part there -> `PR type=hull|mesh lib= sec=` / `box` / `sphere` / `capsule` (one
    //     body, a compound when several; the `_colN` entities go, the geometry does not change);
    //   * own-section trimeshes (shape=3 without `lib=`): per section, every F-line showing it
    //     agrees -> `PR type=mesh sec=`, or `BO complexity=complex` when every section of the Mesh
    //     has one (by its library identity);
    //   * a body and its own section's trimesh on one part, a rotated primitive (a C rotation
    //     composes inside the entity scale, a compound child's outside), a body on a non-first
    //     part, disagreeing placements, prims whose trigger / layer differ, or a Mesh that already
    //     has a BO record keeps its C records (`refused`, with why).
    // Migrated C lines are removed; V records (placement volumes) stay. Idempotent; written
    // (temp + rename) only when @p apply and something changed.
    static bool MigrateModelCollision(const std::string& iscnPath, bool apply, CollisionMigrationReport& report,
                                      LibraryIdentityCache& libraries, std::string* outError = nullptr);

    // What a model's C records give each of its parts - the baseline an MC line of a
    // scene is compared against to tell an edit from an echo (Resource::
    // StripColliderEchoes). One row per F-line that resolves to a .meshlib entry:
    // the part's library (the loader's spelling: the model's folder joined with
    // `file=`), its entry (key first, as the loader resolves it) and, when the
    // F-line has a C record the spawn accepts, the collider the part gets (its
    // first accepted C record; the body is RigidBodyComponent{Static}).
    static bool ReadPartColliderDefaults(const std::string& iscnPath, std::vector<PartColliderDefault>& out,
                                         LibraryIdentityCache& libraries);

    // ---- Editing a model's asset collision, and what is placed of it ----
    // Replaces the BO / PR records of @p setups' Meshes (every other Mesh's stay) with theirs,
    // before the ME block (an Empty() setup: none - its records just go); the file is written
    // (temp + rename) only when the text changes. @p outOldText / @p outNewText: the whole text
    // before and after - an undo writes one back with WriteModelText.
    static bool ReplaceModelCollision(const std::string& iscnPath, const std::vector<Resource::MeshBodySetup>& setups,
                                      std::string& outOldText, std::string& outNewText,
                                      std::string* outError = nullptr);
    static bool WriteModelText(const std::string& iscnPath, const std::string& text, std::string* outError = nullptr);
    // As ReplaceModelCollision, for the MK / MP records (Resource/ModelShapeKeys.h): @p setups'
    // Meshes get theirs (an Empty() setup: none, its records just go), before the ME block.
    static bool ReplaceModelShapeKeys(const std::string& iscnPath,
                                      const std::vector<Resource::MeshShapeKeySetup>& setups,
                                      std::string& outOldText, std::string& outNewText,
                                      std::string* outError = nullptr);

    // ---- The model's sockets (its SO records) ----
    // A spawned instance publishes them: the skeleton sockets become derived entries of the
    // skeleton root's SocketComponent (added when the model has any), each bone resolved by name;
    // one whose bone the skeleton lacks is not published (one warning names it), and the entity's
    // own socket of the same name wins. The root's derived list is replaced (a refresh after an
    // edit); the serializer never writes a derived socket. Returns the sockets published.
    static uint32_t DeriveModelSockets(World& world, Entity skeletonRoot, const SkeletonAsset& skeleton,
                                       const Resource::ModelSockets& sockets, const std::string& modelPath);
    // The same for every placed instance of @p iscnPath (each SceneRef root naming it, a prefab's
    // too), from the model file's current records (the Skeleton Editor's Save). Returns the
    // instances refreshed.
    static uint32_t RefreshModelSockets(World& world, const SkeletonRegistry& skeletons, const std::string& iscnPath);
    // Every SO record of the model becomes @p sockets' (in their order, before the ME
    // block); written (temp + rename) only when the text changes. @p outOldText /
    // @p outNewText: as ReplaceModelCollision's (an undo writes one back).
    static bool ReplaceModelSockets(const std::string& iscnPath, const std::vector<Resource::ModelSocket>& sockets,
                                    std::string& outOldText, std::string& outNewText,
                                    std::string* outError = nullptr);
    // Every VB record of the model becomes @p bones' (Resource/ModelVirtualBones.h), before the ME
    // block, as ReplaceModelSockets. A placed instance takes them at its next spawn (they change
    // its skeleton).
    static bool ReplaceModelVirtualBones(const std::string& iscnPath,
                                         const std::vector<Resource::ModelVirtualBone>& bones,
                                         std::string& outOldText, std::string& outNewText,
                                         std::string* outError = nullptr);
    // Both at once, in one write (the Skeleton Editor's Save): the VB records, then the skeleton
    // socket records, before the ME block. A mesh socket (`SO mesh=`) belongs to the Mesh Editor:
    // its lines stay as the file has them, and a mesh socket in @p sockets is not written (the
    // Skeleton Editor passes its whole loaded list; a stale copy must not undo the other editor's
    // save).
    static bool ReplaceModelSkeletonRecords(const std::string& iscnPath,
                                            const std::vector<Resource::ModelSocket>& sockets,
                                            const std::vector<Resource::ModelVirtualBone>& bones,
                                            std::string& outOldText, std::string& outNewText,
                                            std::string* outError = nullptr);
    // The mesh socket records of one Mesh (`SO mesh=<key>`) become @p sockets' (each must be a
    // socket of @p meshKey), before the ME block; every other SO record stays. The Mesh Editor's
    // Save. As ReplaceModelSockets.
    static bool ReplaceMeshSockets(const std::string& iscnPath, uint64_t meshKey,
                                   const std::vector<Resource::ModelSocket>& sockets,
                                   std::string& outOldText, std::string& outNewText,
                                   std::string* outError = nullptr);
    // A StaticMesh owner of the Mesh @p meshKey publishes that Mesh's sockets (the model's
    // `SO mesh=<key>` records) as derived entries of its SocketComponent, flagged kMeshSocket
    // (SocketSystem: offset x the owner's world); the entity's own socket of the same name wins;
    // the derived list is replaced (none: emptied, and no component is added for nothing).
    // Returns the sockets published.
    static uint32_t DeriveMeshSockets(World& world, Entity owner, const Resource::ModelSockets& sockets,
                                      uint64_t meshKey, const std::string& modelPath);
    // The placed instances of @p iscnPath (SceneRef roots naming it, a prefab's too) whose
    // skeleton was loaded with other VB records than the model file has now
    // (SkeletonAsset::virtualBoneSignature); they take the file's at their next spawn (a scene
    // reload).
    static uint32_t CountStaleVirtualBones(World& world, const SkeletonRegistry& skeletons, const std::string& iscnPath);

    // The model file has a `C` record (per-F-line collision it has not migrated to
    // BO / PR): its asset collision cannot be edited until it is (the spawn gives a
    // part its C collider first).
    static bool ModelHasCRecords(const std::string& iscnPath);
    // Gives the placed instances of @p iscnPath (every SceneRef root naming it, a prefab's too)
    // the model's current asset collision, by the spawn's rules: a part's collider the asset gave
    // it and nobody changed since goes, the new one comes (and becomes its ColliderSpawnDefault).
    // A part whose collider was edited keeps it. Returns the parts that changed. A StaticMesh
    // owner follows on its own once MeshAssetRegistry::InvalidateModel is told.
    static uint32_t RefreshAssetCollision(World& world, const std::string& iscnPath);

    // ---- Read-only placement view (asset preview) ----
    // Parses a .iscn's N / F / C / V lines into one placement per (node, accepted mesh ref), keyed
    // by the DFS position LoadPrefab uses. Read-only, through AssetFS, so it works on a packed
    // build. Returns false only when the file cannot be read or is not a scene container; a scene
    // with no F-lines succeeds with an empty vector.
    // No World, no MeshLibrary, no GPU upload, no GuidRegistry: the asset-preview window builds
    // one Synthetic draw packet per part from the result, and a spawn (or a private World) would
    // drag in GuidRegistry's single-World hook and MeshDescriptorHeap's OnWorldClear FlushAndWait
    // for a picture.
    // Only the first root is read: SceneInstanceLoader::Load returns the first root it spawned and
    // LoadPrefab runs its DFS from exactly that entity, so a second root's subtree has no override
    // slots at all. A file with more than one root says so once per session.
    static bool ReadMeshPlacements(const std::string&          iscnPath,
                                   std::vector<MeshPlacement>& out);

    // The SectionKey of the part a placement spawns: the F-line's key when its library has it (the
    // loader follows it), else the key of its entry (Resource::MeshLibKeyTableAt); 0 when the
    // library does not read. What Resource::PrefabPartKeyOf reads off the spawned entity.
    static uint64_t PlacementSectionKey(const MeshPlacement& mp);

    // ---- Skeleton and preview reads ----
    // True when the model names a skeleton (a `K` line). Which of its Meshes are skinned is
    // Resource::MeshCountsAsSkinned: the identity's own flag, and this only for an index-derived
    // identity, which never sets that flag. Read through AssetFS; false when the file does not
    // read.
    static bool NamesSkeleton(const std::string& iscnPath);

    // ---- Every file a .iscn names (the asset catalog) ----
    // The F-line `file=` and `mat=`, the K-line skeleton, the M-line morph files and the collision
    // libraries (a C / V record's `lib=`, a PR prim's `lib=`), parsed by the same ParseFLine /
    // ParseKLine / ParseCLine code Load() uses (so a material name with spaces is one reference,
    // as it is to the loader) and resolved against the .iscn's directory as Load() resolves them.
    // From text the caller already holds: no file read.
    static std::vector<FileReference> ListFileReferences(const std::string& text,
                                                         const std::string& iscnPath);

    // ---- The identity of a .meshlib that carries none ----
    // The identity the backfill tool would write into @p blob (the bytes of the .meshlib at
    // @p meshlibPath), key for key: Resource::SynthesizeMeshIdentity, grouped the way the model's
    // .iscn places the library - one Mesh per N-line's set of entries in this file, in node
    // order - with each section named after its F-line's `name=` (else its material's file stem).
    // @p iscnPath defaults to the same-stem .iscn next to the library, the model's main file. When
    // there is none (a `_collision.meshlib`), every entry becomes its own one-section Mesh. Read
    // through AssetFS, so it works on a packed build; never logs a missing .iscn.
    static Resource::MeshIdentity SynthesizeMeshLibIdentity(const std::string& meshlibPath,
                                                            const uint8_t*     blob,
                                                            size_t             size,
                                                            const std::string& iscnPath = {});
};
