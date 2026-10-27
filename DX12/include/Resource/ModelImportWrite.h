#pragma once

// ModelImportWrite - the one place a model importer's output (PMX, VRM, assimp, the Unreal scene
// importer) reaches disk.
//
// A re-import must not destroy authored state: an existing .imat holds the artist's material, and
// the .iscn F-line `mat=` is the ONLY durable record of which material each mesh part uses
// (SaveScene skips every .iscn-spawned entity, and nothing else writes a .iscn from the live world).
// The policy has its own translation unit, outside the editor GUI, so every importer shares it by
// construction and a test harness can link and drive it without the import menu.

#include "Resource/MeshLibIdentity.h"
#include "Resource/ModelCollision.h"   // MeshBodySetup (a source's collision)
#include "Resource/ModelShapeKeys.h"   // MeshShapeKeySetup (a source's presets)

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    // One emitted file. `blob` is BORROWED - it must outlive the call.
    struct ImportFileOut
    {
        std::string                 relativePath;   // relative to outDir, forward slashes
        const std::vector<uint8_t>* blob = nullptr;
    };

    // Collision an importer found in the source for ONE Mesh - UCX_ / UBX_ / USP_ / UCP_ nodes,
    // Godot's -col / -convcol - in the Mesh's own space. Named by the Mesh's identity SOURCE key: its
    // MeshKey is known only once the writer has adopted the library's keys. A hull prim's points ride
    // along: the writer appends them to the model's collision library (<stem>_collision.meshlib beside
    // the .iscn, Resource::CollisionLibrary - the same points get the same key on every re-import) and
    // fills its lib= / sec=.
    struct ImportedCollision
    {
        std::string                                 meshSourceKey;
        std::string                                 meshName;     // the log; the hull owner's name
        MeshBodySetup                               setup;        // meshKey: filled by the writer
        std::vector<std::vector<DirectX::XMFLOAT3>> hullPoints;   // [prim]: a hull's points, else empty
    };

    // Shape-key records an importer found in the source for ONE Mesh - presets (VRM expressions, PMX
    // group morphs) and the targets' default weights (glTF mesh.weights) - named by the Mesh's identity
    // SOURCE key like ImportedCollision. Every record carries its `source` (src=), which lets the next
    // re-import tell them from the author's.
    struct ImportedShapeKeys
    {
        std::string       meshSourceKey;
        std::string       meshName;   // the log
        MeshShapeKeySetup setup;      // meshKey: filled by the writer
    };

    // The import dialog's options - the model's IM record, which a re-import applies again unless
    // told otherwise:
    //   IM normals=import|compute smooth=<deg> morphs=1|0 morphNormals=auto|compute|none
    //      collision=names|none|box|convex|complex materials=create|reuse textures=1|0 flipGreen=0|1
    //      animations=1|0 meshlets=1|0 lod=1|0
    struct ModelImportOptions
    {
        enum class Normals      : uint8_t { Import, Compute };
        enum class MorphNormals : uint8_t { Auto, Compute, None };
        enum class Collision    : uint8_t { Names, None, Box, Convex, Complex };
        enum class Materials    : uint8_t { Create, Reuse };

        // compute: the source's normals are dropped and made again, smoothed
        // across edges up to smoothAngle degrees (assimp's GenSmoothNormals).
        // A format without normals gets them made either way.
        Normals      normals      = Normals::Import;
        float        smoothAngle  = 60.f;
        bool         morphs       = true;                // shape keys at all
        // auto: the source's normal deltas where they turn a normal, else computed; compute: always
        // computed; none: no normal deltas.
        MorphNormals morphNormals = MorphNormals::Auto;
        // names: the source's UCX_ / UBX_ / USP_ / UCP_ nodes and -col / -convcol; none: those shapes
        // dropped (the nodes still do not draw); box / convex / complex: made for every static Mesh that
        // gets no collision from the source or the author - its bounds, the hull of its vertices, its own
        // triangles.
        Collision    collision    = Collision::Names;
        // reuse: a source material whose name one project .imat already has
        // (exactly one) binds that one instead of a new Materials/<name>.imat.
        Materials    materials    = Materials::Create;
        bool         textures     = true;                // cook the source's textures
        bool         flipGreen    = false;               // a DirectX-convention source's normal maps
        // The source's animation clips (skeletal and shape-key channels) as <stem>/<stem>.ianim - bound
        // as it is by Animator.SetClip, in Game too.
        bool         animations   = true;
        // meshlets: the model library's MLET chunk for its static sections; lod: their discrete LOD
        // levels.
        bool         meshlets     = true;
        bool         lod          = true;

        bool operator==(const ModelImportOptions&) const = default;
    };
    std::string FormatImportOptions(const ModelImportOptions& options);   // one line, '\n'-ended
    // The first IM line of a model's text; false (and @p out the defaults) without one.
    bool ParseImportOptions(const std::string& modelText, ModelImportOptions& out);
    bool ReadImportOptions(const std::string& iscnPath, ModelImportOptions& out);
    // One option by its IM name and value ("normals", "compute"); false with
    // @p error on an unknown name or value - the IM parser and Lua share it.
    bool SetImportOption(ModelImportOptions& options, const std::string& name, const std::string& value,
                         std::string* error = nullptr);

    // What an import is asked beyond "read this source", handed to the importers.
    struct ModelImportContext
    {
        // The project-relative folder the files land in - what the materials' texture references are
        // spelled against. "" = no textures cooked.
        std::string materialRoot;
        // The model's name: its folder, its files and its model key. "" = the
        // source's file stem. A re-import keeps the model's own, whatever the
        // source file is called now.
        std::string stem;
        ModelImportOptions options;
    };

    // The model's source: the `SR` record of its .iscn, written by every import and read by a
    // re-import -
    //   SR path=<pct> hash=<16 hex> importer=<assimp|pmx|vrm|ue>
    // The path is project-relative when the source is under the project, else absolute; '/'
    // separators. NOT an asset reference: nothing scans, moves or packages it (a source routinely
    // lives outside the project).
    struct ModelSourceRecord
    {
        std::string path;
        uint64_t    hash = 0;   // FNV-1a 64 of the source's bytes (MeshIdentityHash) when it was imported
        std::string importer;
        bool Valid() const { return !path.empty(); }
    };
    std::string FormatModelSource(const ModelSourceRecord& source);   // one line, '\n'-ended
    bool        ParseModelSource(const std::string& modelText, ModelSourceRecord& out);   // its first SR line
    bool        ReadModelSource(const std::string& iscnPath, ModelSourceRecord& out);

    struct ModelImportStats
    {
        int written          = 0;   // files actually written
        int failed           = 0;   // files that could not be written
        int keptMaterials    = 0;   // existing .imat left untouched
        int restoredBindings = 0;   // F-line mat= values carried over from the old .iscn
        // Meshes given the source's collision, hulls newly appended to the collision library, and Meshes
        // whose authored collision (the old .iscn's BO / PR) was carried over.
        int collisionMeshes  = 0;
        int collisionHulls   = 0;
        int keptCollision    = 0;
        // Meshes gone from the new library whose authored collision was kept as orphan=1 (a Mesh that
        // comes back gets it back).
        int orphanCollision  = 0;
        // Meshes whose shape-key records (MK / MP) were carried over, and those kept as orphan=1 (their
        // Mesh is gone from the new library).
        int keptShapeKeys    = 0;
        int orphanShapeKeys  = 0;
        // The source's own records (src=) written, over how many Meshes.
        int sourceShapeKeyMeshes = 0;
        int sourcePresets        = 0;
        int sourceDefaults       = 0;
        // The source's textures written (<stem>/Textures/*.itex).
        int textures             = 0;
        // Meshes given made collision (collision = box / convex / complex), and source materials bound to
        // a project .imat of the same name (reuse).
        int madeCollision        = 0;
        int reusedMaterials      = 0;
        // The previous file's authored sockets (SO) carried over, those kept as orphan=1 (the new
        // skeleton lacks their bone), and the source's (src=) dropped: no importer reports sockets to
        // replace them.
        int keptSockets          = 0;
        int orphanSockets        = 0;
        int droppedSourceSockets = 0;
        // Libraries given an MLET chunk, and their clusters (every level).
        int      meshletLibraries = 0;
        uint32_t meshletClusters  = 0;
        // Mesh/Section keys carried over from the previous .meshlib: the counts summed over every library
        // written, the lists (the re-import's report) the model library's only. All zero on a first
        // import.
        MeshIdentityAdoption identity;
    };

    // How WriteModelImportFiles treats the previous files.
    struct ModelWriteOptions
    {
        const ModelSourceRecord*       source  = nullptr;   // written as the model's SR record
        const std::vector<MeshRename>* renames = nullptr;   // explicit: see MeshAdoptOptions
        bool geometryRenames = true;   // a gone Mesh and a new one whose geometry agrees keep one key
        bool dryRun          = false;  // stats.identity only: what the re-import would do - nothing is written
        // The options the import ran with - written as the IM record, and the writer's part of them
        // (collision box / convex / complex / none, materials = reuse). Null: the defaults, no IM record.
        const ModelImportOptions*      importOptions = nullptr;
    };

    // Write @p files under @p outDirUtf8, PRESERVING authored state:
    //
    //   .imat   SKIPPED when the file already exists (the never-overwrite rule of
    //           CreateMaterialInstance). Written LAST, after the textures they name, with their text
    //           re-augmented then - a texture's guid line needs the file.
    //   .itex   the source's textures: derived, overwritten.
    //   .iscn   written (mesh-entry indices really do change with the source), then the PREVIOUS
    //           file's material bindings are re-applied on top, and its asset collision - the BO / PR
    //           records of every Mesh whose key survived and that the source gives no collision of its
    //           own (@p collision replaces the rest), so a Mesh Editor's authored collision survives a
    //           re-import. A Mesh gone from the new library keeps its records as orphan=1; one that
    //           comes back gets them back. Last, the SR and IM records (@p options).
    //   .meshlib  overwritten, but its identity layer first ADOPTS the previous file's Mesh/Section
    //           keys (AdoptMeshIdentityKeys): a re-import that reorders the source must not re-key what
    //           scenes refer to. A previous file without an identity is read through the one the
    //           backfill would give it (SynthesizeMeshLibIdentity over the OLD .iscn), so its keys
    //           survive too. A library that draws gets an MLET chunk for its static sections (unless
    //           meshlets=0).
    //   everything else (.iskel / .imorph)  plain overwrite - derived data, the source is
    //           authoritative (the .imorph's blocks take the adopted section keys first).
    //   MK / MP the previous file's AUTHORED records (no src=) come back - a gone Mesh's as orphan=1 -
    //           and @p shapeKeys (the source's own, src=) fill in every name the author has no record
    //           of; the previous file's src= records are the source's and are replaced.
    //   SO / VB the previous file's authored sockets and virtual bones come back, orphan=1 when the new
    //           skeleton (for a mesh socket, the new library) lacks what they name; src= sockets are
    //           dropped.
    //
    // The re-apply matches an old F-line to a new one by its section key first, then on `name=` (the
    // authoring tool's material name, k-th occurrence to k-th), and falls back to `mesh=` ONLY when the
    // old F-line has no name. Do not key the merge on meshId: adding, removing or reordering one
    // material shifts every meshId and would silently bind materials to the WRONG parts; keys and names
    // survive all three edits.
    //
    // @p label only names the importer in the log line. @p options: the SR and IM records, how Meshes
    // renamed in the source keep their keys, and the dry run a re-import's review is made from.
    ModelImportStats WriteModelImportFiles(const std::string&                     outDirUtf8,
                                           const std::vector<ImportFileOut>&      files,
                                           const char*                            label,
                                           const std::vector<ImportedCollision>*  collision = nullptr,
                                           const std::vector<ImportedShapeKeys>*  shapeKeys = nullptr,
                                           const ModelWriteOptions*               options   = nullptr);

    // The .meshlib half of the policy above, for an importer that writes its
    // own files (UnrealSceneImporter): give @p freshBlob - a library the
    // importer just built WITH an identity - the keys of the library now at
    // @p outPath, read from its IDNT or, when it has none, synthesized from its
    // same-stem .iscn. RUN IT BEFORE THAT .iscn IS OVERWRITTEN. Returns false
    // and leaves @p freshBlob untouched when there is nothing to adopt from or
    // into; @p stats (optional) gets the counts and the report.
    bool AdoptExistingMeshLibKeys(const std::string& outPath, std::vector<uint8_t>& freshBlob,
                                  MeshIdentityAdoption* stats = nullptr,
                                  const MeshAdoptOptions* options = nullptr);
}
