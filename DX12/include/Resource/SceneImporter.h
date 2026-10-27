#pragma once

// SceneImporter - offline cook for multi-mesh 3D model files.
//
// Takes a source model file (glTF / FBX / OBJ / ...) and produces a set of
// internal-format blobs ready to write to disk, all under `<stem>/`:
//
//   <stem>.meshlib   - ALL geometry: one entry per non-empty aiMesh (Assimp
//                      splits a node's mesh by material, so an entry is one
//                      material SECTION), shared vertex/index pools
//   <stem>.iscn      - the model's main file: scene graph + section bindings
//   <stem>.iskel     - skeleton + per-section skin weights (rigged sources)
//   <stem>.imorph    - morph target deltas (rigged sources with blend shapes)
//   Materials/*.imat - one per aiMaterial
//
// .iscn text payload format (one record per line; the parser is
// SceneInstanceLoader, which dispatches on a single letter + space):
//   # comment
//   S nodes=<N> meshfiles=<M> source=<original_filename>
//   N idx=<i> parent=<j|-1> tx=.. sz=<f> mc=<k> [mr0=<fi> mr1=<fi> ...] name=<to end of line>
//   F idx=<i> file=<.meshlib> mesh=<entry> [name=<pct>] [mat=<.imat to end of line>]
//   K file=<.iskel>          M idx=<F idx> file=<.imorph>
//
// Fields:
//   N  - node record (one per aiNode)   mr0..mrN - indices into F records
//   F  - one section: which library entry, its material slot name and default material
//   Key order inside N / F is part of the grammar: name= (N) and mat= (F) run
//   to the end of the line, so nothing may be written after them.
//
// The index spaces: F idx == .meshlib entry id == "submesh" index, i.e. the
// position among NON-EMPTY aiMeshes. The .iskel per-mesh blend records and the
// .imorph blocks are keyed in the same space.

#include "Resource/HumanoidBone.h"
#include "Resource/ModelImportWrite.h"   // ImportedCollision

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

struct aiMesh;

namespace Resource
{
    class SceneImporter
    {
    public:
        // Optional humanoid role hint: (canonical role, source node/bone name).
        // When supplied (e.g. from a VRM humanoid map), the importer builds the
        // skeleton's HumanoidProfile from this map instead of name auto-detection.
        using HumanoidHint = std::vector<std::pair<HumanoidBone, std::string>>;
        // A preset the caller read from the source's own extension data
        // (VrmImporter: a VRM expression) - a named set of weights over shape keys,
        // by the target NAMES the model's .imorph gives them. Import hands each Mesh
        // the part its sections' targets carry (ImportResult::shapeKeys).
        struct PresetHint
        {
            std::string                                name;
            std::vector<std::pair<std::string, float>> weights;   // target -> 0..1
            std::string                                source;    // its records' src= ("vrm")
        };
        struct ExportedFile
        {
            std::string          relativePath;  // "<stem>/<file>", e.g. "char/char.meshlib"
            std::vector<uint8_t> blob;
        };

        struct ImportResult
        {
            std::vector<ExportedFile> files;    // .imat files, .meshlib, .imorph, .iskel, then the .iscn
            bool                      success = false;
            // The source's collision nodes (UCX_ / UBX_ / USP_ / UCP_, Godot's -col /
            // -convcol) as each Mesh's asset collision - for WriteModelImportFiles, which
            // keys and writes it.
            std::vector<ImportedCollision> collision;
            // The source's shape-key records per Mesh - the caller's presets, cut to the
            // targets each Mesh carries, and a glTF's default weights (mesh.weights, as
            // MK default=) - for WriteModelImportFiles likewise.
            std::vector<ImportedShapeKeys> shapeKeys;
        };

        // Import a model from raw source bytes.
        // @p sourcePath: full filesystem path (extension hint + output naming);
        // @p sourceData: raw file bytes. Returns all output blobs; the caller writes
        // them to the asset directory.
        // @p context: its materialRoot is the project-relative folder the files will
        // be written under - given, the materials take the source's factors and
        // textures (cooked into <stem>/Textures/, named by that folder); without it
        // they stay default-constructed. Its stem names the model instead of the
        // source's file name (a re-import from a renamed source).
        static ImportResult Import(const std::string&             sourcePath,
                                   const std::vector<uint8_t>&    sourceData,
                                   const HumanoidHint*            humanoidHint = nullptr,
                                   HumanoidConvention             humanoidConv = HumanoidConvention::Unknown,
                                   const std::vector<PresetHint>* presets      = nullptr,
                                   const ModelImportContext*      context      = nullptr);

        // Build a .meshlib blob from N aiMesh inputs (one MeshLibraryEntry per
        // aiMesh, shared vertex/index pools). Public so batch pipelines
        // (UnrealSceneImporter) can combine meshes from MULTIPLE Assimp scenes
        // into one library - the packer re-bases indices internally; callers
        // only need every aiMesh pointer to stay alive until this returns.
        // aiMaterialIdx[i] lands in MeshLibraryEntry::defaultMaterialIdx.
        //
        // @p mirrorAxes (optional, one entry per source) bakes an axis mirror into
        // the packed vertices: bit0 = negate X, bit1 = Y, bit2 = Z. Positions,
        // normals and tangents are negated per axis, and when the mirror has a
        // NEGATIVE determinant (an odd number of axes) the triangle winding is
        // reversed and the tangent handedness inverted so the result still
        // front-faces correctly. Lets a caller emit a mirrored variant of a
        // mesh instead of handing the renderer a negative-scale instance.
        // The same source aiMesh may appear several times with different masks.
        // @p flipX is a whole-library shorthand for mask bit0.
        //
        // @p preTransforms (optional, one entry per source) is applied to the
        // vertices BEFORE the mirror: positions by the full affine, normals and
        // tangents by the linear part (renormalised). A negative-determinant
        // transform reverses the winding and flips tangent handedness, exactly
        // like a mirror mask. This bakes a SKINNED mesh's node transform into the
        // geometry, so the .meshlib entry is model-space bind geometry and the
        // skeleton does not depend on which node drew it - see
        // SkeletonImportResult::meshNodeGlobal. Identity entries cost nothing.
        static std::vector<uint8_t> BuildMeshLibBlob(
            const std::vector<const aiMesh*>& sources,
            const std::vector<uint32_t>&      aiMaterialIdx,
            bool                              flipX      = false,
            const std::vector<uint8_t>*       mirrorAxes = nullptr,
            const std::vector<DirectX::XMFLOAT4X4>* preTransforms = nullptr);

        // Source extensions this importer handles.
        static std::vector<const char*> GetSourceExtensions()
        {
            return { ".obj", ".gltf", ".glb", ".fbx", ".dae", ".3ds", ".ply", ".stl", ".vrm" };
        }
    };
}
