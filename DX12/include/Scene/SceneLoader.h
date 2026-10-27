#pragma once

// SceneLoader: owns SkinnedMeshPending, the skinned-mesh CPU data that
// SceneInstanceLoader decodes and Renderer::RegisterSkinnedMeshFull uploads.
// Scenes are imported offline by SceneImporter; there is no runtime Assimp load.

#include "ECS/ECS.h"
#include "Resource/SkeletonAsset.h"  // BlendVertex, kInvalidSkeletonIndex

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

class SceneLoader
{
public:
    // Per-skinned-mesh CPU data for Renderer::RegisterSkinnedMeshFull(), filled by
    // SceneInstanceLoader from a .iskel blob.
    struct SkinnedMeshPending
    {
        Entity   entity         = NullEntity;
        Entity   rootEntity     = NullEntity;
        uint32_t skeletonIndex  = kInvalidSkeletonIndex;

        // Content identity of the source geometry below, or 0 for "unique".
        // Every stream here is immutable asset data of (mesh source, meshId, .iskel,
        // .imorph). When non-zero, SkinnedMeshSubsystem keeps one set of source buffers
        // per key for the world's lifetime and hands every later instance the same
        // handles; only the skinning output and the mesh descriptor slot stay per
        // instance. The caller must guarantee that equal keys mean byte-identical streams.
        uint64_t sourceKey = 0;

        std::vector<DirectX::XMFLOAT3> restPositions;
        std::vector<DirectX::XMFLOAT3> restNormals;
        std::vector<DirectX::XMFLOAT2> uvs;
        std::vector<DirectX::XMFLOAT2> uvs1;          // optional 2nd UV set (SDF face / lightmap)
        std::vector<DirectX::XMFLOAT2> uvs2;          // optional additional UV set (float2)
        std::vector<DirectX::XMFLOAT2> uvs3;          // optional additional UV set (float2)
        std::vector<DirectX::XMFLOAT4> tangents;     // optional
        std::vector<uint32_t>           indices;
        std::vector<BlendVertex>        blendData;

        // Morph targets. morphCount is the mesh's LOCAL target count (the shapes that
        // move this mesh), not the skeleton-wide list length (see
        // MORPHLIB_FLAG_SPARSE_SLOTS in Resource/AssetHeader.h). morphDeltas is the GPU
        // delta buffer's image: morphCount x vertexCount position deltas, target-major
        // [m * vertexCount + v], then, when morphHasNormals, as many normal deltas in the
        // same order (.imorph v2).
        std::vector<DirectX::XMFLOAT3> morphDeltas;
        uint32_t                        morphCount = 0;
        bool                            morphHasNormals = false;
        std::vector<std::string>        morphNames;
        // Local target index -> skeleton morph index; size == morphCount. Always
        // populated: for a dense .imorph without a slot table the loader synthesises the
        // identity mapping, so every consumer has one path.
        std::vector<uint32_t>           morphSlotOfLocal;

        // The caller found sourceKey's shared source live
        // (Renderer::HasSharedSkinnedSource) and skipped decoding: every stream above is
        // empty. Only the shared path accepts such a part, so a shared source gone in
        // between is an error, never an empty-mesh upload.
        bool                            sharedOnly = false;
    };
};
