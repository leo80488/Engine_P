#pragma once
// Resource/ModelVirtualBones.h - a model's VIRTUAL BONES: the `VB` lines of its main .iscn.
//   VB name=<pct> from=<pct bone> to=<pct bone> [orphan=1]
// A virtual bone is a bone no vertex is skinned to and no DCC file has: its parent is `from`, and
// every frame its model-space transform is `to`'s in the animated pose (UE's virtual bones). Uses:
// an IK target in another bone's space (VB root_foot_l: the foot's animated place in root space),
// a socket's bone ("with the hand, but in root space"), a Lua query.
//
// The skeleton: at load (SceneInstanceLoader) ApplyVirtualBones appends the records AFTER the real
// bones of the .iskel - [realBoneCount, boneCount) - with parent = from, rest = to's rest place
// under from, bind = to's rest model matrix, no grant, no AABB. They never go into the .iskel.
// AnimationSystem sets their pose after the pre-IK grants (EvaluateVirtualBones), before IK, so a
// later IK / HeadTrack / chain that moves an ancestor of the VB moves it too, as in UE.
// Rules: both bones are REAL bones (never another VB), from != to, one VB per (from, to) pair, a
// name no bone has. A record that breaks one is skipped with a warning; `orphan=1` marks one whose
// bone a re-import lost (kept, not applied).

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SkeletonAsset;

namespace Resource
{
    struct ModelVirtualBone
    {
        std::string name;
        std::string from;
        std::string to;
        bool        orphan = false;
        bool operator==(const ModelVirtualBone& o) const
        {
            return name == o.name && from == o.from && to == o.to && orphan == o.orphan;
        }
        bool operator!=(const ModelVirtualBone& o) const { return !(*this == o); }
    };

    struct ModelVirtualBones
    {
        std::vector<ModelVirtualBone> bones;   // in file order
        const ModelVirtualBone* Find(const std::string& name) const;
    };

    // The VB records of a model file's TEXT. A malformed record is skipped and
    // described in @p warnings (optional); a repeated name keeps the LAST record.
    void ParseModelVirtualBones(const std::string& text, ModelVirtualBones& out,
                                std::vector<std::string>* warnings = nullptr);
    // The model file's records, read through AssetFS and cached per path (re-read
    // when the loose file's size / write time changes). Never null. Thread-safe.
    std::shared_ptr<const ModelVirtualBones> ModelVirtualBonesAt(const std::string& iscnPath);

    // One record, ending in "\n" ("" for a nameless one).
    std::string FormatVirtualBone(const ModelVirtualBone& vb);
    std::string FormatVirtualBones(const std::vector<ModelVirtualBone>& list);
    // "" = a legal name (1..63 bytes - SkeletonAsset::boneNames' 64), else why not.
    std::string VirtualBoneNameProblem(const std::string& name);
    // UE's default: "VB <from>_<to>" (cut, UTF-8 safe, to 63 bytes).
    std::string DefaultVirtualBoneName(const std::string& from, const std::string& to);

    // Append @p vbs to @p skel (fresh from its .iskel: no virtual bone yet) - see the file comment.
    // Returns how many were appended; each record skipped says why in @p warnings (optional).
    // @p skel.realBoneCount is set either way.
    uint32_t ApplyVirtualBones(SkeletonAsset& skel, const std::vector<ModelVirtualBone>& vbs,
                               std::vector<std::string>* warnings = nullptr);

    // A signature of the records that change a skeleton (the BatchScope share key):
    // 0 = none.
    uint64_t VirtualBonesSignature(const std::vector<ModelVirtualBone>& vbs);
}
