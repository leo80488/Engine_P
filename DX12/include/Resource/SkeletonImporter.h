#pragma once

// SkeletonImporter - Assimp-based extraction of the skeleton, the animation clips and the
// per-mesh blend weights of an aiScene. SceneImporter calls Import() once per source model; the
// result becomes the .iskel and the skinned meshes' blend data.
// Editor-only (it needs Assimp), except Fnv32 and BuildGrantTable: SkeletonGrantTable.cpp (EngineCore)
// defines those for both exes.

#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

struct aiScene;
struct aiMesh;
struct aiNode;
struct aiAnimation;

namespace Resource
{
    // Output of one SkeletonImporter::Import call.
    struct SkeletonImportResult
    {
        SkeletonAsset             skeleton;
        std::vector<ClipAsset>    clips;           // one per aiAnimation in the scene

        // Per aiMesh index: blend weights packed as BlendVertex[vertexCount].
        // Index matches scene->mMeshes[i]. Empty vector = the mesh is not skinned (no bones, no rigid binding).
        std::vector<std::vector<BlendVertex>> perMeshBlendData;

        // Per aiMesh index: the referencing node's MODEL-space transform G, the product of
        // mTransformation from the node up to (but EXCLUDING) the scene root. Row-vector convention:
        // v_model = v_mesh * G.
        // Assimp's FBX aiBone::mOffsetMatrix maps MESH-local -> bone-local, so a skeleton shared by
        // mesh nodes with different transforms (a Blender/VRChat export: Body at identity, hair 1.26 m
        // up) gets a DIFFERENT offset matrix for the same bone from every mesh. The importer removes G
        // from the offset and bakes it into the vertex data instead, which makes the skeleton
        // mesh-independent. Identity for meshes that are not skinned (nothing is baked there).
        std::vector<DirectX::XMFLOAT4X4> meshNodeGlobal;
        std::vector<uint8_t>             meshIsSkinned;   // 1 = aiMesh has bones

        // True when Import found (or synthesized) a skeleton.
        bool hasBones = false;

        // An aiMesh with no bones whose shape keys move something is skinned RIGIDLY (every vertex to
        // one bone at weight 1), so its morphs run on the skinned path like any other:
        //   * in a model with a skeleton, to the nearest bone at or above its node - meshIsSkinned is
        //     set, so it is baked into model space and follows that bone like a skinned mesh;
        //   * in a model without one, to a SYNTHESIZED one-bone skeleton (synthesizedSkeleton; the bone
        //     is the identity at rest and nothing animates it) - meshIsSkinned stays 0: the vertices
        //     stay in the mesh's own space, the node keeps its transform (and its collision), and the
        //     identity skin draws them where a static mesh would be.
        bool     synthesizedSkeleton = false;
        uint32_t rigidMeshes         = 0;     // aiMeshes bound rigidly
    };

    // Stateless; every member is static.
    class SkeletonImporter
    {
    public:
        static constexpr float kMinShapeMotion = 1e-4f;

        // The synthesized skeleton's one bone. Deliberately not a name the
        // runtime reads anything into (ChainPhysics auto-attaches to bones named
        // like hair / skirt / chest).
        static constexpr const char* kRigidBoneName = "RigidRoot";

        // Parse bones and animations from an already-loaded aiScene. False when the scene has no bones
        // (neither mesh bones nor an animated node hierarchy) and no bone-less mesh whose shape keys
        // move. On success fills every field of @p result.
        static bool Import(const aiScene* scene, SkeletonImportResult& result);

        // Fill grantSource / grantRatio (the PMX grant: a bone that copies another bone's rotation) by
        // naming convention. An MMD D-bone (a Japanese name + ASCII 'D', e.g. the left-leg D-bone)
        // copies the bone without the 'D' at ratio 1; EX bones get no explicit grant. Needs nameToIndex.
        static void BuildGrantTable(SkeletonAsset& skel);

        // Does any of @p mesh's shape keys (aiAnimMesh) move a vertex by more than kMinShapeMotion on
        // some axis? The one bar for "this mesh has shape keys": the rigid skin
        // (SkeletonImportResult::synthesizedSkeleton) and SceneImporter's morph blocks share it.
        static bool ShapeKeysMove(const aiMesh* mesh);

    private:
        // Collect every bone node referenced by any mesh (plus their non-bone ancestors, or the animated
        // hierarchy when there is no mesh bone) in root-first order (parentIndex[i] < i). False when
        // there is none. @p nodeGlobal maps every aiNode to its model-space transform (see
        // SkeletonImportResult::meshNodeGlobal); @p meshNodeGlobal, per aiMesh, is stripped out of
        // aiBone::mOffsetMatrix.
        static bool CollectBones(const aiScene*                        scene,
                                  SkeletonAsset&                        out,
                                  std::unordered_map<std::string, uint32_t>& nameToSlot,
                                  const std::unordered_map<const aiNode*, DirectX::XMFLOAT4X4>& nodeGlobal,
                                  const std::vector<DirectX::XMFLOAT4X4>& meshNodeGlobal);

        // Walk the aiNode hierarchy recursively, giving each node in @p boneNodeSet a bone slot and its
        // parent, rest pose and bind pose.
        static void WalkBoneHierarchy(const aiNode*                          node,
                                       int32_t                                parentSlot,
                                       const std::unordered_map<std::string, bool>& boneNodeSet,
                                       SkeletonAsset&                         out,
                                       std::unordered_map<std::string, uint32_t>& nameToSlot,
                                       const std::unordered_map<const aiNode*, DirectX::XMFLOAT4X4>& nodeGlobal);

        // Per-mesh BlendVertex arrays: the MAX_BONES_PER_VERTEX heaviest influences per vertex, weights
        // normalised and quantised to [0,255]. A vertex left with no usable influence is pinned to
        // @p fallbackBone at full weight: an all-zero weight tuple is undefined for the dual-quaternion
        // blend in Skin.cs.
        static void BuildBlendData(const aiScene*                                scene,
                                    const std::unordered_map<std::string, uint32_t>& nameToSlot,
                                    std::vector<std::vector<BlendVertex>>&            outPerMesh,
                                    uint32_t                                          fallbackBone);

        // Convert one aiAnimation to a ClipAsset SOA sampled at 30 fps. Bones without a channel keep
        // @p skeleton's rest-pose TRS.
        static ClipAsset BuildClip(const aiAnimation*              anim,
                                    const std::unordered_map<std::string, uint32_t>& nameToSlot,
                                    uint32_t                               boneCount,
                                    const SkeletonAsset&                   skeleton);

        // FNV-32 hash for bone names.
        static uint32_t Fnv32(const char* s);
    };
}
