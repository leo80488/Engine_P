#pragma once

// PmxImporter - a binary parser for PMX 2.0/2.1 model files, used instead of Assimp for PMX for
// direct access to the grant (MMD's append transform) and IK data.
//
// Coordinate convention: identity (no axis negation). MMD and DX12 are both left-handed Y-up; the
// Z direction (toward the viewer or into the screen) is a camera convention, not a model-space
// difference. Positions, normals, quaternions and triangle winding pass through unchanged.

#include "Resource/ModelImportWrite.h"   // ImportedShapeKeys (a group morph's preset)
#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{
    // Parsed material properties, one per PMX material entry.
    struct PmxMaterial
    {
        std::string        nameJP;                   // material name (Japanese)
        std::string        nameEN;                   // material name (English, may be empty)
        DirectX::XMFLOAT4 diffuse   = { 1,1,1,1 };
        DirectX::XMFLOAT3 specular  = { 0,0,0 };
        float              shininess = 0.f;
        DirectX::XMFLOAT3 ambient   = { 0,0,0 };
        int32_t            textureIndex = -1;      // index into textureFiles[]
        int32_t            sphereTextureIndex = -1;
        uint8_t            sphereMode = 0;         // 0=disabled, 1=mul, 2=add, 3=sub
        int32_t            toonTextureIndex = -1;
        uint32_t           indexStart = 0;
        uint32_t           indexCount = 0;
        uint8_t            drawFlags  = 0;         // bit 0 = no culling (two-sided)
    };

    // A vertex morph: a position delta per vertex.
    struct PmxMorphTarget
    {
        char                            name[64]{};
        // Sparse: only modified vertices have deltas.
        std::vector<uint32_t>           vertexIndices;  // global PMX vertex indices
        std::vector<DirectX::XMFLOAT3>  positionDeltas; // corresponding position offsets
    };

    // A group morph: other morphs at ratios. Imported as a preset.
    struct PmxGroupMorph
    {
        std::string                            name;      // Japanese name
        std::vector<std::pair<int32_t, float>> offsets;   // (PMX morph index, ratio)
    };

    // What PmxImporter::Import returns; success is false when the file did not parse.
    struct PmxImportResult
    {
        SkeletonAsset skeleton;

        // Mesh data: all submeshes concatenated, split by the materials' index ranges.
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        // First three PMX additional UV sets (.xy of each float4).
        // Each vector stays empty when that source channel is absent.
        std::vector<DirectX::XMFLOAT2> uv1;
        std::vector<DirectX::XMFLOAT2> uv2;
        std::vector<DirectX::XMFLOAT2> uv3;
        std::vector<uint32_t>           indices;
        std::vector<BlendVertex>        blendData;

        // Per-material face ranges and properties
        std::vector<PmxMaterial>        materials;

        // Texture filenames referenced by materials
        std::vector<std::string>        textureFiles;

        // Vertex morphs (blend shapes / facial expressions)
        std::vector<PmxMorphTarget>     morphTargets;

        // Every PMX morph in file order - its type (0 group, 1 vertex, 2 bone, 3-7 UV, 8 material,
        // 9 flip, 10 impulse) and, for a vertex / group morph, its index in morphTargets / groupMorphs (-1
        // for the rest): what a group morph's offsets index.
        std::vector<uint8_t>            morphTypes;
        std::vector<int32_t>            morphSlots;
        std::vector<PmxGroupMorph>      groupMorphs;

        bool success = false;
    };

    // One emitted file; the same layout as SceneImporter::ExportedFile.
    struct PmxExportedFile
    {
        std::string           relativePath;
        std::vector<uint8_t>  blob;
    };

    // Stateless: every member is static.
    class PmxImporter
    {
    public:
        // Parse a PMX file from an in-memory buffer.
        static PmxImportResult Import(const uint8_t* data, size_t size);

        // Convenience: load from file path.
        static PmxImportResult ImportFromFile(const std::string& path);

        // Build the .meshlib / .iskel / .imorph / .imat / .iscn blobs (and the cooked textures) from a
        // successful PmxImportResult. `stem` names them ("ModelName" -> "ModelName/ModelName.iscn").
        // Returns relative-path blobs ready to be written to disk.
        // @p outShapeKeys (optional): the group morphs as presets of the model's one Mesh, for
        // WriteModelImportFiles.
        // @p sourcePath / @p materialRoot: the .pmx (its folder is where the texture table's paths start)
        // and the project-relative folder the files will be written under - given both, the materials take
        // the PMX diffuse colour, texture and two-sidedness.
        static std::vector<PmxExportedFile> BuildBlobs(const PmxImportResult&          pmx,
                                                        const std::string&              stem,
                                                        std::vector<ImportedShapeKeys>* outShapeKeys = nullptr,
                                                        const std::string*              sourcePath   = nullptr,
                                                        const std::string*              materialRoot = nullptr);

        // The morphs = 0 import option: @p pmx as if the file had no morphs - no shape keys, no presets,
        // no morph names in the skeleton.
        static void DropMorphs(PmxImportResult& pmx);
    };
}
