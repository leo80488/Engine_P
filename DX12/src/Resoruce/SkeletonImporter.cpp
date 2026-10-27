
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/SkeletonImporter.h"
#include "System/Log.h"

#include <assimp/scene.h>
#include <assimp/anim.h>

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace
{
    // Convert Assimp column-vector matrix -> DX row-vector matrix (transpose).
    XMFLOAT4X4 ToXM(const aiMatrix4x4& m)
    {
        XMFLOAT4X4 o;
        o._11 = m.a1; o._12 = m.b1; o._13 = m.c1; o._14 = m.d1;
        o._21 = m.a2; o._22 = m.b2; o._23 = m.c2; o._24 = m.d2;
        o._31 = m.a3; o._32 = m.b3; o._33 = m.c3; o._34 = m.d3;
        o._41 = m.a4; o._42 = m.b4; o._43 = m.c4; o._44 = m.d4;
        return o;
    }

    // Multiply two XMFLOAT4X4 (row-vector convention).
    XMFLOAT4X4 Mul(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
    {
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&a), XMLoadFloat4x4(&b)));
        return out;
    }

    XMFLOAT4X4 Inv(const XMFLOAT4X4& m)
    {
        XMFLOAT4X4 out;
        XMVECTOR det;
        XMStoreFloat4x4(&out, XMMatrixInverse(&det, XMLoadFloat4x4(&m)));
        return out;
    }

    XMFLOAT4X4 Identity()
    {
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, XMMatrixIdentity());
        return out;
    }

    // The product of all ancestor transforms of @p node, the scene root INCLUDED (unlike
    // BuildNodeGlobals); only the fallback for a node nodeGlobal lacks.
    XMFLOAT4X4 GetNodeGlobalTransform(const aiNode* node)
    {
        if (!node) return Identity();
        XMFLOAT4X4 local = ToXM(node->mTransformation);
        if (!node->mParent) return local;
        return Mul(GetNodeGlobalTransform(node->mParent), local);
    }

    // MODEL SPACE CONTRACT (whole importer): "model space" is the frame of .iscn node 0, EXCLUDING
    // node 0's own mTransformation. SceneInstanceLoader puts node 0's TRS on the spawned root
    // entity's LocalTransform, so folding it in here would apply it twice whenever a RootNode carries
    // a unit-scale / axis-conversion transform. bindPose, inverseBindPose, the restPoseLocal chain,
    // the bone rest AABBs and the baked vertex data all live in this one space.
    // Iterative, not recursive: deep single-child bone chains are common in these rigs. The DFS
    // mirrors SceneImporter::CollectNodes.
    void BuildNodeGlobals(const aiNode* sceneRoot,
                          std::unordered_map<const aiNode*, XMFLOAT4X4>& out)
    {
        if (!sceneRoot) return;
        out[sceneRoot] = Identity();   // node 0's own TRS is deliberately excluded
        std::vector<const aiNode*> stack{ sceneRoot };
        while (!stack.empty())
        {
            const aiNode* n = stack.back();
            stack.pop_back();
            const XMFLOAT4X4 parentG = out[n];
            for (unsigned i = 0; i < n->mNumChildren; ++i)
            {
                const aiNode* c = n->mChildren[i];
                out[c] = Mul(parentG, ToXM(c->mTransformation));
                stack.push_back(c);
            }
        }
    }

    // Find an aiNode by name, searching the whole scene tree.
    const aiNode* FindNode(const aiNode* root, const std::string& name)
    {
        if (!root) return nullptr;
        if (root->mName.C_Str() == name) return root;
        for (unsigned i = 0; i < root->mNumChildren; ++i)
        {
            const aiNode* found = FindNode(root->mChildren[i], name);
            if (found) return found;
        }
        return nullptr;
    }

    // Mark bone nodes for an ANIMATION-ONLY scene (no meshes => Assimp creates
    // no aiBones, but the node hierarchy + channels fully describe the rig).
    // A node is a bone when it is animated, an ANCESTOR of an animated node
    // (armature wrappers), or a DESCENDANT of one (unanimated tip bones like
    // "HeadTop_End"/"LeftToe_End" still define bone directions). The scene root
    // itself is excluded. Returns true if the subtree contains an animated node.
    bool MarkAnimatedHierarchy(const aiNode*                                node,
                               bool                                         underAnimated,
                               const std::unordered_map<std::string, bool>& animated,
                               std::unordered_map<std::string, bool>&       outBones,
                               const aiNode*                                sceneRoot)
    {
        const bool self  = animated.count(node->mName.C_Str()) != 0;
        const bool inSub = underAnimated || self;

        bool childHasAnimated = false;
        for (unsigned i = 0; i < node->mNumChildren; ++i)
            childHasAnimated |= MarkAnimatedHierarchy(node->mChildren[i], inSub,
                                                      animated, outBones, sceneRoot);

        if ((inSub || childHasAnimated) && node != sceneRoot)
            outBones[node->mName.C_Str()] = true;

        return self || childHasAnimated;
    }
}

namespace Resource
{
    // DFS: bone slots are assigned root-first, so parentIndex[i] < i.
    void SkeletonImporter::WalkBoneHierarchy(
        const aiNode*                                   node,
        int32_t                                         parentSlot,
        const std::unordered_map<std::string, bool>&    boneNodeSet,
        SkeletonAsset&                                  out,
        std::unordered_map<std::string, uint32_t>&      nameToSlot,
        const std::unordered_map<const aiNode*, XMFLOAT4X4>& nodeGlobal)
    {
        const std::string name = node->mName.C_Str();

        int32_t mySlot = parentSlot; // carry parent's slot if this node is not a bone

        if (boneNodeSet.count(name))
        {
            if (nameToSlot.count(name) == 0 && out.boneCount < SkeletonAsset::MAX_BONES)
            {
                const uint32_t slot = out.boneCount++;
                nameToSlot[name]    = slot;

                out.parentIndex[slot] = parentSlot;

				strncpy_s(out.boneNames[slot], sizeof(out.boneNames[0]), name.c_str(), _TRUNCATE);
                out.boneNames[slot][sizeof(out.boneNames[0]) - 1] = '\0';

                out.restPoseLocal[slot] = ToXM(node->mTransformation);

                // bindPose = the bone node's model-space transform. Every non-bone ancestor was promoted to a
                // bone (CollectBones), so chaining restPoseLocal through parentIndex reproduces exactly this
                // matrix: AnimationSystem relies on that for "skin == identity at rest".
                const auto it = nodeGlobal.find(node);
                out.bindPose[slot] = (it != nodeGlobal.end()) ? it->second
                                                              : GetNodeGlobalTransform(node);

                // CollectBones replaces this with aiBone::mOffsetMatrix for every bone a mesh references.
                out.inverseBindPose[slot] = Inv(out.bindPose[slot]);

                mySlot = static_cast<int32_t>(slot);
            }
        }

        for (unsigned i = 0; i < node->mNumChildren; ++i)
            WalkBoneHierarchy(node->mChildren[i], mySlot, boneNodeSet, out, nameToSlot, nodeGlobal);
    }

    bool SkeletonImporter::CollectBones(
        const aiScene*                             scene,
        SkeletonAsset&                             out,
        std::unordered_map<std::string, uint32_t>& nameToSlot,
        const std::unordered_map<const aiNode*, XMFLOAT4X4>& nodeGlobal,
        const std::vector<XMFLOAT4X4>&             meshNodeGlobal)
    {
        // Collect the set of all bone names referenced by any mesh.
        std::unordered_map<std::string, bool> boneNodeSet;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
                boneNodeSet[mesh->mBones[bi]->mName.C_Str()] = true;
        }

        // Promote every non-bone ANCESTOR of a bone to a bone. Blender/VRChat FBX exports wrap the rig
        // in an "Armature" node that carries the +90 deg X axis conversion and that no aiBone
        // references. Left out, Hips would be a ROOT bone whose restPoseLocal is relative to Armature
        // while its bindPose includes it: the composed rest pose would be rotated -90 deg about X
        // against the bind pose and no skin matrix would be identity at rest. Promoting the wrapper keeps
        // restPose == bindPose by construction and clip channels node-local (no composition math); the
        // animation-only path gets the same from MarkAnimatedHierarchy.
        if (!boneNodeSet.empty())
        {
            std::unordered_map<std::string, const aiNode*> byName;
            {
                std::vector<const aiNode*> stack{ scene->mRootNode };
                while (!stack.empty())
                {
                    const aiNode* n = stack.back(); stack.pop_back();
                    if (!n) continue;
                    byName.emplace(n->mName.C_Str(), n);
                    for (unsigned i = 0; i < n->mNumChildren; ++i)
                        stack.push_back(n->mChildren[i]);
                }
            }
            std::vector<std::string> seeds;
            seeds.reserve(boneNodeSet.size());
            for (const auto& [nm, _] : boneNodeSet) seeds.push_back(nm);

            uint32_t promoted = 0;
            for (const std::string& nm : seeds)
            {
                auto it = byName.find(nm);
                if (it == byName.end()) continue;
                for (const aiNode* p = it->second->mParent;
                     p && p != scene->mRootNode; p = p->mParent)
                {
                    const std::string pn = p->mName.C_Str();
                    if (boneNodeSet.count(pn)) break;   // ancestors above are already in
                    boneNodeSet[pn] = true;
                    ++promoted;
                }
            }
            if (promoted > 0)
                LOG_INFO("SkeletonImporter: promoted %u wrapper node(s) to bones "
                         "(non-bone ancestors of skinned bones)", promoted);
        }

        // ANIMATION-ONLY fallback (e.g. a Mixamo FBX downloaded "without skin"): no meshes means no
        // aiBones, but the node hierarchy + animation channels fully describe the rig. The bone set is
        // the animated nodes plus their ancestors and descendants; WalkBoneHierarchy derives rest/bind
        // poses from node transforms, no aiBone offsets needed. The .iskel is a valid RETARGET SOURCE
        // (skeleton + clips + humanoid profile) that carries no skinning data.
        if (boneNodeSet.empty() && scene->mNumAnimations > 0)
        {
            std::unordered_map<std::string, bool> animated;
            for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai)
            {
                const aiAnimation* anim = scene->mAnimations[ai];
                for (unsigned c = 0; c < anim->mNumChannels; ++c)
                    animated[anim->mChannels[c]->mNodeName.C_Str()] = true;
            }
            if (!animated.empty())
            {
                MarkAnimatedHierarchy(scene->mRootNode, false, animated,
                                      boneNodeSet, scene->mRootNode);
                if (!boneNodeSet.empty())
                    LOG_INFO("SkeletonImporter: no mesh bones — built %zu-bone skeleton "
                             "from the animated node hierarchy (animation-only import)",
                             boneNodeSet.size());
            }
        }

        if (boneNodeSet.empty())
            return false;


        out.boneCount = 0;

        // DFS from the scene root: bones are stored root-first, so parentIndex[i] < i.
        WalkBoneHierarchy(scene->mRootNode, -1, boneNodeSet, out, nameToSlot, nodeGlobal);
        if (boneNodeSet.size() > out.boneCount)
            LOG_WARNING("SkeletonImporter: %zu bone nodes but only %u slots taken "
                        "(MAX_BONES=%u) — the rig is truncated",
                        boneNodeSet.size(), out.boneCount, SkeletonAsset::MAX_BONES);

        // inverseBindPose comes from aiBone::mOffsetMatrix (more accurate than inverting the node
        // transform: Assimp post-processing may have shifted the geometry), with the MESH-NODE TRANSFORM
        // taken out first. For FBX, Assimp builds mOffsetMatrix as inverse(TransformLink) *
        // meshNodeGlobal, i.e. MESH-local -> bone-local, so a rig shared by mesh nodes with different
        // transforms (Body at the origin, hair 1.26 m up) would get a different bind pose from every
        // mesh. Removing G here and baking it into the vertex data instead
        // (SceneImporter::BuildMeshLibBlob preTransforms) makes the skeleton mesh-independent:
        // v_model * invBind == v_mesh * offset.
        std::vector<int32_t> firstMeshForBone(SkeletonAsset::MAX_BONES, -1);
        uint32_t conflictBones = 0;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            const XMFLOAT4X4 G = (mi < meshNodeGlobal.size()) ? meshNodeGlobal[mi] : Identity();
            const XMFLOAT4X4 invG = Inv(G);
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            {
                const aiBone* bone = mesh->mBones[bi];
                const std::string name = bone->mName.C_Str();
                auto it = nameToSlot.find(name);
                if (it == nameToSlot.end()) continue;
                const uint32_t slot = it->second;
                const XMFLOAT4X4 invBind = Mul(invG, ToXM(bone->mOffsetMatrix));

                if (firstMeshForBone[slot] >= 0)
                {
                    // Two meshes still disagree about the same bone after G is removed: a genuinely inconsistent
                    // export. Warn and keep the first.
                    float d = 0.f;
                    const float* a = &invBind._11;
                    const float* b = &out.inverseBindPose[slot]._11;
                    for (int k = 0; k < 16; ++k) d = (std::max)(d, std::fabs(a[k] - b[k]));
                    if (d > 1e-3f)
                    {
                        ++conflictBones;
                        LOG_WARNING("SkeletonImporter: bone '%s' has conflicting bind poses "
                                    "(mesh %d '%s' vs mesh %u '%s', max delta %.4f) — keeping the first",
                                    name.c_str(), firstMeshForBone[slot],
                                    scene->mMeshes[firstMeshForBone[slot]]->mName.C_Str(),
                                    mi, mesh->mName.C_Str(), d);
                    }
                    continue;   // first mesh wins, deterministically
                }
                firstMeshForBone[slot]    = static_cast<int32_t>(mi);
                out.inverseBindPose[slot] = invBind;
                // Recompute bindPose as inverse of inverseBindPose for consistency.
                out.bindPose[slot] = Inv(out.inverseBindPose[slot]);
            }
        }
        if (conflictBones > 0)
            LOG_WARNING("SkeletonImporter: %u bone(s) had conflicting bind poses across meshes",
                        conflictBones);

        // Populate nameToIndex hash map (FNV-32 keys).
        for (uint32_t i = 0; i < out.boneCount; ++i)
            out.nameToIndex[Fnv32(out.boneNames[i])] = i;

        return out.boneCount > 0;
    }

    void SkeletonImporter::BuildBlendData(
        const aiScene*                                scene,
        const std::unordered_map<std::string, uint32_t>& nameToSlot,
        std::vector<std::vector<BlendVertex>>&            outPerMesh,
        uint32_t                                          fallbackBone)
    {
        outPerMesh.resize(scene->mNumMeshes);
        uint32_t orphanVerts = 0;

        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            if (mesh->mNumBones == 0) continue;

            const uint32_t nv = mesh->mNumVertices;
            outPerMesh[mi].assign(nv, BlendVertex{});

            // Every influence of each vertex; trimmed to the MAX_BONES_PER_VERTEX heaviest and packed below.
            struct RawInfluence { uint32_t boneIdx; float weight; };
            std::vector<std::vector<RawInfluence>> raw(nv);

            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi)
            {
                const aiBone* bone = mesh->mBones[bi];
                auto it = nameToSlot.find(bone->mName.C_Str());
                if (it == nameToSlot.end()) continue;
                const uint32_t boneSlot = it->second;

                for (unsigned wi = 0; wi < bone->mNumWeights; ++wi)
                {
                    const unsigned vi = bone->mWeights[wi].mVertexId;
                    if (vi >= nv) continue;
                    raw[vi].push_back({ boneSlot, bone->mWeights[wi].mWeight });
                }
            }

            for (uint32_t vi = 0; vi < nv; ++vi)
            {
                auto& inf = raw[vi];

                // Keep the MAX_BONES_PER_VERTEX heaviest.
                if (inf.size() > MAX_BONES_PER_VERTEX)
                {
                    std::partial_sort(inf.begin(), inf.begin() + MAX_BONES_PER_VERTEX, inf.end(),
                        [](const RawInfluence& a, const RawInfluence& b) {
                            return a.weight > b.weight;
                        });
                    inf.resize(MAX_BONES_PER_VERTEX);
                }

                float sum = 0.f;
                for (auto& r : inf) sum += r.weight;
                if (sum > 1e-6f)
                    for (auto& r : inf) r.weight /= sum;

                BlendVertex& bv = outPerMesh[mi][vi];
                uint32_t packedSum = 0;
                for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k)
                {
                    if (k < static_cast<uint32_t>(inf.size()))
                    {
                        const uint32_t idx = std::min(inf[k].boneIdx, 65535u);
                        bv.boneIndices[k] = static_cast<uint16_t>(idx);
                        bv.boneWeights[k] = static_cast<uint8_t>(std::min(inf[k].weight * 255.f + 0.5f, 255.f));
                        packedSum += bv.boneWeights[k];
                    }
                    // Unused slots keep BlendVertex{}'s zero index and weight.
                }

                // A vertex with NO usable influence (none, or every weight rounded to 0/255) would reach
                // Skin.cs with an all-zero weight tuple: the dual-quaternion blend normalises 0/0 and the vertex
                // collapses to the origin or goes NaN, taking its triangle with it. Pin it rigidly to the root
                // bone instead: that skin matrix is identity at rest, so the vertex stays where it was authored.
                if (packedSum == 0)
                {
                    bv.boneIndices[0] = static_cast<uint16_t>(fallbackBone);
                    bv.boneWeights[0] = 255;
                    ++orphanVerts;
                }
            }
        }

        if (orphanVerts > 0)
            LOG_WARNING("SkeletonImporter: %u vertex(es) had no usable bone influence — "
                        "pinned to bone %u so DQS blending stays defined",
                        orphanVerts, fallbackBone);
    }

    ClipAsset SkeletonImporter::BuildClip(
        const aiAnimation*                              anim,
        const std::unordered_map<std::string, uint32_t>& nameToSlot,
        uint32_t                                        boneCount,
        const SkeletonAsset&                            skeleton)
    {
        ClipAsset clip;
        clip.boneCount = boneCount;

        const double ticksPerSec = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 30.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        // Sample at a fixed 30 fps.
        constexpr float kFrameRate = 30.f;
        const uint32_t frameCount = std::max(2u, static_cast<uint32_t>(durationSec * kFrameRate) + 1);

        clip.frameCount = frameCount;
        clip.duration   = static_cast<float>(durationSec);
        clip.frameRate  = kFrameRate;

        // Every bone starts at its rest-pose local TRS in every frame: a bone with no channel must keep
        // its bind-pose offset from its parent, not collapse to the parent's origin.
        clip.positions.resize(static_cast<size_t>(boneCount) * frameCount);
        clip.rotations.resize(static_cast<size_t>(boneCount) * frameCount);
        clip.scales.resize   (static_cast<size_t>(boneCount) * frameCount);

        for (uint32_t bi = 0; bi < boneCount; ++bi)
        {
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            XMFLOAT3 restScl = { 1.f, 1.f, 1.f };

            XMVECTOR sc, rot, tr;
            if (XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skeleton.restPoseLocal[bi])))
            {
                XMStoreFloat3(&restPos, tr);
                XMStoreFloat4(&restRot, rot);
                XMStoreFloat3(&restScl, sc);
            }

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(bi) * frameCount + fi;
                clip.positions[idx] = restPos;
                clip.rotations[idx] = restRot;
                clip.scales[idx]    = restScl;
            }
        }

        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
        {
            const aiNodeAnim* ch = anim->mChannels[ci];
            const std::string name = ch->mNodeName.C_Str();
            auto it = nameToSlot.find(name);
            if (it == nameToSlot.end()) continue;
            const uint32_t boneIdx = it->second;

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const double t = (static_cast<double>(fi) / (frameCount - 1)) * anim->mDuration;

                auto samplePos = [&]() -> XMFLOAT3
                {
                    if (ch->mNumPositionKeys == 0) return { 0.f, 0.f, 0.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumPositionKeys; ++k)
                        if (t < ch->mPositionKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumPositionKeys)
                    {
                        auto& v = ch->mPositionKeys[k].mValue;
                        return { v.x, v.y, v.z };
                    }
                    const double t0 = ch->mPositionKeys[k].mTime;
                    const double t1 = ch->mPositionKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mPositionKeys[k].mValue;
                    auto& b = ch->mPositionKeys[k + 1].mValue;
                    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
                };

                auto sampleRot = [&]() -> XMFLOAT4
                {
                    if (ch->mNumRotationKeys == 0) return { 0.f, 0.f, 0.f, 1.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumRotationKeys; ++k)
                        if (t < ch->mRotationKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumRotationKeys)
                    {
                        auto& q = ch->mRotationKeys[k].mValue;
                        return { q.x, q.y, q.z, q.w };
                    }
                    const double t0 = ch->mRotationKeys[k].mTime;
                    const double t1 = ch->mRotationKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mRotationKeys[k].mValue;
                    auto& b = ch->mRotationKeys[k + 1].mValue;
                    aiQuaternion out;
                    aiQuaternion::Interpolate(out, a, b, f);
                    return { out.x, out.y, out.z, out.w };
                };

                auto sampleScl = [&]() -> XMFLOAT3
                {
                    if (ch->mNumScalingKeys == 0) return { 1.f, 1.f, 1.f };
                    uint32_t k = 0;
                    for (; k + 1 < ch->mNumScalingKeys; ++k)
                        if (t < ch->mScalingKeys[k + 1].mTime) break;
                    if (k + 1 >= ch->mNumScalingKeys)
                    {
                        auto& v = ch->mScalingKeys[k].mValue;
                        return { v.x, v.y, v.z };
                    }
                    const double t0 = ch->mScalingKeys[k].mTime;
                    const double t1 = ch->mScalingKeys[k + 1].mTime;
                    const float  f  = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.f;
                    auto& a = ch->mScalingKeys[k].mValue;
                    auto& b = ch->mScalingKeys[k + 1].mValue;
                    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
                };

                const size_t idx = static_cast<size_t>(boneIdx) * frameCount + fi;
                clip.positions[idx] = samplePos();
                clip.rotations[idx] = sampleRot();
                clip.scales[idx]    = sampleScl();
            }
        }

        return clip;
    }

    bool SkeletonImporter::ShapeKeysMove(const aiMesh* mesh)
    {
        if (!mesh || !mesh->mVertices || mesh->mNumFaces == 0) return false;
        for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a)
        {
            const aiAnimMesh* am = mesh->mAnimMeshes[a];
            if (!am || !am->mVertices || am->mNumVertices != mesh->mNumVertices) continue;
            for (unsigned v = 0; v < mesh->mNumVertices; ++v)
            {
                const aiVector3D d = am->mVertices[v] - mesh->mVertices[v];
                if (std::fabs(d.x) > kMinShapeMotion || std::fabs(d.y) > kMinShapeMotion ||
                    std::fabs(d.z) > kMinShapeMotion)
                    return true;
            }
        }
        return false;
    }

    bool SkeletonImporter::Import(const aiScene* scene, SkeletonImportResult& result)
    {
        if (!scene) return false;

        // Model-space transform of every node (node 0's own TRS excluded, see BuildNodeGlobals), and per
        // aiMesh the one of the node that references it.
        std::unordered_map<const aiNode*, XMFLOAT4X4> nodeGlobal;
        BuildNodeGlobals(scene->mRootNode, nodeGlobal);

        result.meshNodeGlobal.assign(scene->mNumMeshes, Identity());
        result.meshIsSkinned.assign(scene->mNumMeshes, 0u);
        std::vector<const aiNode*> owner(scene->mNumMeshes, nullptr);   // the node referencing each aiMesh
        {
            std::vector<const aiNode*> stack{ scene->mRootNode };
            while (!stack.empty())
            {
                const aiNode* n = stack.back(); stack.pop_back();
                if (!n) continue;
                for (unsigned k = 0; k < n->mNumMeshes; ++k)
                {
                    const unsigned m = n->mMeshes[k];
                    if (m >= scene->mNumMeshes) continue;
                    if (owner[m])
                    {
                        // One aiMesh instanced under two nodes cannot have a
                        // single baked transform. Take the first and say so.
                        LOG_WARNING("SkeletonImporter: aiMesh[%u] is referenced by both '%s' and "
                                    "'%s' — baking the first node's transform only",
                                    m, owner[m]->mName.C_Str(), n->mName.C_Str());
                        continue;
                    }
                    owner[m] = n;
                    const auto it = nodeGlobal.find(n);
                    if (it != nodeGlobal.end()) result.meshNodeGlobal[m] = it->second;
                }
                // Reverse push so children pop in source order: "the first node" above is then the first one
                // in the file, not the first off a LIFO stack.
                for (unsigned i = n->mNumChildren; i-- > 0; )
                    stack.push_back(n->mChildren[i]);
            }
        }
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            result.meshIsSkinned[m] = (scene->mMeshes[m] && scene->mMeshes[m]->mNumBones > 0) ? 1u : 0u;

        // The bone-less aiMeshes whose shape keys move something; each is skinned rigidly below (see
        // SkeletonImportResult::synthesizedSkeleton).
        std::vector<unsigned> rigid;
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            if (!result.meshIsSkinned[m] && ShapeKeysMove(scene->mMeshes[m])) rigid.push_back(m);

        std::unordered_map<std::string, uint32_t> nameToSlot;
        if (!CollectBones(scene, result.skeleton, nameToSlot,
                          nodeGlobal, result.meshNodeGlobal))
        {
            if (rigid.empty()) return false;
            // No skeleton at all: one bone, the identity at rest and bind, that
            // nothing animates - the skin it gives is the identity.
            SkeletonAsset& sk = result.skeleton;
            sk.boneCount      = 1;
            sk.parentIndex[0] = -1;
            XMStoreFloat4x4(&sk.inverseBindPose[0], XMMatrixIdentity());
            XMStoreFloat4x4(&sk.bindPose[0], XMMatrixIdentity());
            XMStoreFloat4x4(&sk.restPoseLocal[0], XMMatrixIdentity());
            strncpy_s(sk.boneNames[0], sizeof(sk.boneNames[0]), kRigidBoneName, _TRUNCATE);
            sk.nameToIndex.clear();
            sk.nameToIndex[Fnv32(sk.boneNames[0])] = 0;
            nameToSlot[kRigidBoneName] = 0;
            result.synthesizedSkeleton = true;
        }

        result.hasBones = true;

        // After nameToIndex is filled: BuildGrantTable looks bones up by name hash.
        BuildGrantTable(result.skeleton);

        LOG_INFO("SkeletonImporter: found %u bones", result.skeleton.boneCount);

        // Per-mesh blend data. Influence-less vertices fall back to the first ROOT bone (parentIndex
        // < 0), whose skin matrix is identity at rest, so they stay exactly where they were authored.
        uint32_t fallbackBone = 0;
        for (uint32_t b = 0; b < result.skeleton.boneCount; ++b)
            if (result.skeleton.parentIndex[b] < 0) { fallbackBone = b; break; }
        BuildBlendData(scene, nameToSlot, result.perMeshBlendData, fallbackBone);

        // The rigid bindings: every vertex to one bone at full weight.
        for (const unsigned m : rigid)
        {
            const aiMesh* mesh = scene->mMeshes[m];
            uint32_t      bone = fallbackBone;
            if (!result.synthesizedSkeleton)
            {
                // The nearest bone at or above the mesh's node; a mesh outside the
                // skeleton's hierarchy rides its first root. Baked like every
                // skinned mesh, so it follows that bone.
                for (const aiNode* n = owner[m]; n; n = n->mParent)
                    if (const auto it = nameToSlot.find(n->mName.C_Str()); it != nameToSlot.end())
                    {
                        bone = it->second;
                        break;
                    }
                result.meshIsSkinned[m] = 1u;
            }
            BlendVertex bv{};
            bv.boneIndices[0] = static_cast<uint16_t>(bone);
            bv.boneWeights[0] = 255;
            result.perMeshBlendData[m].assign(mesh->mNumVertices, bv);
            ++result.rigidMeshes;
            LOG_INFO("SkeletonImporter: static mesh '%s' has %u shape key(s) -> rigid-skinned to '%s'%s",
                     mesh->mName.C_Str(), mesh->mNumAnimMeshes, result.skeleton.boneNames[bone],
                     result.synthesizedSkeleton ? " (a one-bone skeleton: the model has none)" : "");
        }

        result.clips.reserve(scene->mNumAnimations);
        for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai)
        {
            ClipAsset clip = BuildClip(scene->mAnimations[ai], nameToSlot,
                                        result.skeleton.boneCount, result.skeleton);
            const char* clipName = scene->mAnimations[ai]->mName.C_Str();
            LOG_INFO("SkeletonImporter: clip '%s' — %u frames, %.2fs",
                     clipName[0] ? clipName : "(unnamed)", clip.frameCount, clip.duration);
            result.clips.push_back(std::move(clip));
        }

        // Per-bone rest AABBs (frustum culling). SPACE CONTRACT: boxes are in bind-MODEL space, because
        // the renderer's bone-AABB merge transforms them with the per-frame SKIN matrices
        // (inverseBindPose x localToModel), which map bind-model -> posed-model; a bone-LOCAL box would
        // land near the model origin. Mesh space -> bind-model is the mesh node's transform G, exactly
        // what SceneImporter bakes into the packed vertices.
        {
            auto& skel = result.skeleton;

            for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            {
                skel.boneRestAABBs[bi].localMin = {  1e30f,  1e30f,  1e30f };
                skel.boneRestAABBs[bi].localMax = { -1e30f, -1e30f, -1e30f };
            }

            for (uint32_t mi = 0; mi < result.perMeshBlendData.size() && mi < scene->mNumMeshes; ++mi)
            {
                const auto& bd = result.perMeshBlendData[mi];
                if (bd.empty()) continue;
                const aiMesh* mesh = scene->mMeshes[mi];
                if (!mesh || !mesh->mVertices) continue;

                const XMMATRIX G = XMLoadFloat4x4(
                    (mi < result.meshNodeGlobal.size()) ? &result.meshNodeGlobal[mi]
                                                        : &skel.bindPose[0]);   // never taken
                const uint32_t vc = (std::min)(static_cast<uint32_t>(bd.size()), mesh->mNumVertices);
                for (uint32_t vi = 0; vi < vc; ++vi)
                {
                    const BlendVertex& bv = bd[vi];
                    const aiVector3D& vPos = mesh->mVertices[vi];

                    XMVECTOR vModel = XMVector3TransformCoord(
                        XMVectorSet(vPos.x, vPos.y, vPos.z, 1.f), G);
                    XMFLOAT3 lp;
                    XMStoreFloat3(&lp, vModel);

                    for (uint32_t w = 0; w < MAX_BONES_PER_VERTEX; ++w)
                    {
                        if (bv.boneWeights[w] == 0) continue;
                        uint32_t bi = bv.boneIndices[w];
                        if (bi >= skel.boneCount) continue;

                        auto& aabb = skel.boneRestAABBs[bi];
                        aabb.localMin.x = (std::min)(aabb.localMin.x, lp.x);
                        aabb.localMin.y = (std::min)(aabb.localMin.y, lp.y);
                        aabb.localMin.z = (std::min)(aabb.localMin.z, lp.z);
                        aabb.localMax.x = (std::max)(aabb.localMax.x, lp.x);
                        aabb.localMax.y = (std::max)(aabb.localMax.y, lp.y);
                        aabb.localMax.z = (std::max)(aabb.localMax.z, lp.z);
                    }
                }
            }

            // A bone that influences no vertex stays DEGENERATE on purpose: the renderer's merge skips it
            // (its skin lives on other bones). Only boxes that have vertices get the 2 cm pad.
            const float kPad = 0.02f;
            uint32_t validAabbs = 0;
            for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            {
                auto& aabb = skel.boneRestAABBs[bi];
                if (aabb.localMin.x > aabb.localMax.x) continue;
                aabb.localMin.x -= kPad; aabb.localMin.y -= kPad; aabb.localMin.z -= kPad;
                aabb.localMax.x += kPad; aabb.localMax.y += kPad; aabb.localMax.z += kPad;
                ++validAabbs;
            }
            skel.hasBoneAABBs = validAabbs > 0;
            LOG_INFO("SkeletonImporter: computed %u/%u bone rest AABBs (bind-model space)",
                     validAabbs, skel.boneCount);
        }

        return true;
    }
}
