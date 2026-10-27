#include "Resource/SkeletonInspect.h"

#include "Physics/ChainBoneNames.h"       // the runtime's own name rules
#include "Resource/AnimationResource.h"   // BoneNameHash: what clip channels bind by
#include "Resource/HumanoidBone.h"
#include "Resource/HumanoidProfileBuilder.h"   // EnsureHumanoidProfile: the runtime's lazy build
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>

using namespace DirectX;

namespace Resource
{
namespace
{
    // A bone name as the runtime reads it: at most 64 bytes, NUL or not.
    std::string BoneName(const SkeletonAsset& s, uint32_t b)
    {
        return std::string(s.boneNames[b], strnlen(s.boneNames[b], sizeof(s.boneNames[b])));
    }

    std::string LowerAscii(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    // The PMX twist kanji ("neji", as in the arm / wrist twist bones) or "twist" in any ASCII case
    // (UE upperarm_twist_01_l).
    bool NamesTwist(const std::string& name)
    {
        if (name.find("捩") != std::string::npos) return true;
        return LowerAscii(name).find("twist") != std::string::npos;
    }

    void AddIssue(SkeletonReport& r, SkeletonIssueKind k, int32_t bone, int32_t other, std::string detail)
    {
        SkeletonIssue i;
        i.kind     = k;
        i.severity = SkeletonIssueDefaultSeverity(k);
        i.bone     = bone;
        i.other    = other;
        i.detail   = std::move(detail);
        r.issues.push_back(std::move(i));
    }

    XMVECTOR JointOf(const XMFLOAT4X4& m) { return XMVectorSet(m._41, m._42, m._43, 1.f); }
}

const char* EnsureHumanoidProfile(SkeletonAsset& skel)
{
    const auto anyRole = [&skel] {
        for (uint32_t i = 0; i < kHumanoidBoneCount; ++i)
            if (skel.humanoid.boneForRole[i] >= 0) return true;
        return false;
    };
    // AnimationSystem's rule, exactly: a valid profile, or one the user edited - even to nothing
    // (Clear All + Save: no mapping, and it stays so) - is the file's; anything else is built.
    if (skel.humanoid.IsValid() || skel.humanoid.userEdited) return anyRole() ? "file" : "none";
    HumanoidProfileBuilder::Build(skel);
    return anyRole() ? "built" : "none";
}

uint32_t SkeletonReport::Count(SkeletonIssueSeverity s) const
{
    return static_cast<uint32_t>(std::count_if(issues.begin(), issues.end(),
                                               [s](const SkeletonIssue& i) { return i.severity == s; }));
}

uint32_t SkeletonReport::Count(SkeletonIssueKind k) const
{
    return static_cast<uint32_t>(std::count_if(issues.begin(), issues.end(),
                                               [k](const SkeletonIssue& i) { return i.kind == k; }));
}

const char* SkeletonIssueKindName(SkeletonIssueKind k)
{
    switch (k)
    {
    case SkeletonIssueKind::BadParent:           return "bad_parent";
    case SkeletonIssueKind::OrderViolation:      return "order_violation";
    case SkeletonIssueKind::EmptyName:           return "empty_name";
    case SkeletonIssueKind::DuplicateName:       return "duplicate_name";
    case SkeletonIssueKind::NameHashCollision:   return "name_hash_collision";
    case SkeletonIssueKind::NearDuplicateName:   return "near_duplicate_name";
    case SkeletonIssueKind::MirroredRest:        return "mirrored_rest";
    case SkeletonIssueKind::BindRestMismatch:    return "bind_rest_mismatch";
    case SkeletonIssueKind::BadGrant:            return "bad_grant";
    case SkeletonIssueKind::BadIKIndex:          return "bad_ik_index";
    case SkeletonIssueKind::DepthOver64:         return "depth_over_64";
    case SkeletonIssueKind::SkinIndexOutOfRange: return "skin_index_out_of_range";
    case SkeletonIssueKind::HumanoidIncomplete:  return "humanoid_incomplete";
    case SkeletonIssueKind::ForkWithDummyLeaf:   return "fork_with_dummy_leaf";
    case SkeletonIssueKind::Count:               break;
    }
    return "unknown";
}

const char* SkeletonIssueSeverityName(SkeletonIssueSeverity s)
{
    switch (s)
    {
    case SkeletonIssueSeverity::Info:    return "info";
    case SkeletonIssueSeverity::Warning: return "warning";
    case SkeletonIssueSeverity::Error:   return "error";
    }
    return "unknown";
}

SkeletonIssueSeverity SkeletonIssueDefaultSeverity(SkeletonIssueKind k)
{
    switch (k)
    {
    case SkeletonIssueKind::BadParent:
    case SkeletonIssueKind::OrderViolation:
    case SkeletonIssueKind::NameHashCollision:
    case SkeletonIssueKind::BadGrant:
    case SkeletonIssueKind::BadIKIndex:
    case SkeletonIssueKind::SkinIndexOutOfRange:
        return SkeletonIssueSeverity::Error;
    case SkeletonIssueKind::EmptyName:
    case SkeletonIssueKind::DuplicateName:
    case SkeletonIssueKind::MirroredRest:
    case SkeletonIssueKind::DepthOver64:
        return SkeletonIssueSeverity::Warning;
    default:
        return SkeletonIssueSeverity::Info;
    }
}

std::vector<const char*> SkeletonBoneFlagNames(uint32_t flags)
{
    static const char* const kNames[] = {
        "root", "leaf", "ik_bone", "ik_target", "ik_link", "grant_source", "grant_target", "twist",
        "weighted", "dummy_leaf", "chain_name", "chain_attach", "humanoid", "mirrored",
        "non_uniform_scale", "virtual",
    };
    std::vector<const char*> out;
    for (uint32_t bit = 0; bit < std::size(kNames); ++bit)
        if (flags & (1u << bit)) out.push_back(kNames[bit]);
    return out;
}

SkeletonReport InspectSkeleton(const SkeletonAsset& skel, const std::vector<SkinWeightStream>& weights)
{
    SkeletonReport r;
    const uint32_t n = (std::min)(skel.boneCount, SkeletonAsset::MAX_BONES);
    r.bones.resize(n);
    r.stats.bones = n;
    const auto validParent = [&](uint32_t b) {
        const int32_t p = skel.parentIndex[b];
        return p >= 0 && p < static_cast<int32_t>(n) && p != static_cast<int32_t>(b);
    };

    // ---- 1. hierarchy: parents, order, children, depth ----
    std::vector<std::vector<uint32_t>> children(n);
    for (uint32_t b = 0; b < n; ++b)
    {
        SkeletonBoneInfo& bi = r.bones[b];
        const int32_t p = skel.parentIndex[b];
        bi.parent = p;
        if (p == -1)
        {
            bi.flags |= kSkelBoneRoot;
            ++r.stats.roots;
            continue;
        }
        if (!validParent(b))
        {
            AddIssue(r, SkeletonIssueKind::BadParent, static_cast<int32_t>(b), -1,
                     "parentIndex " + std::to_string(p) + " is not a bone of this skeleton (" +
                     std::to_string(n) + " bones)");
            continue;
        }
        if (p > static_cast<int32_t>(b))
            AddIssue(r, SkeletonIssueKind::OrderViolation, static_cast<int32_t>(b), p,
                     "listed before its parent [" + std::to_string(p) + "]: a forward pass composes it "
                     "from a parent it has not computed yet");
        children[static_cast<uint32_t>(p)].push_back(b);
    }
    for (uint32_t b = 0; b < n; ++b)
    {
        SkeletonBoneInfo& bi = r.bones[b];
        bi.childCount = static_cast<uint16_t>((std::min)(children[b].size(), std::size_t{ 0xFFFF }));
        if (children[b].empty())
        {
            bi.flags |= kSkelBoneLeaf;
            ++r.stats.leaves;
        }
        // Ancestors, bounded: a cycle (only possible through an order violation)
        // stops at n steps and is a bad parent, not a hang.
        uint32_t depth = 0;
        uint32_t cur   = b;
        while (validParent(cur) && depth <= n)
        {
            cur = static_cast<uint32_t>(skel.parentIndex[cur]);
            ++depth;
        }
        if (depth > n)
        {
            AddIssue(r, SkeletonIssueKind::BadParent, static_cast<int32_t>(b), -1,
                     "its parent chain loops (never reaches a root)");
            depth = n;
        }
        bi.depth = static_cast<uint16_t>((std::min)(depth, 0xFFFFu));
        r.stats.maxDepth = (std::max)(r.stats.maxDepth, depth);
        // IKSystem / HeadTrack collect the bone plus its ancestors into chain[64].
        if (depth >= 64)
            AddIssue(r, SkeletonIssueKind::DepthOver64, static_cast<int32_t>(b), -1,
                     std::to_string(depth) + " ancestors: the 64-deep model-space walks (IK, HeadTrack) "
                     "stop short of the root");
    }

    // ---- 2. names ----
    std::unordered_map<uint32_t, uint32_t>    byHash;
    std::unordered_map<std::string, uint32_t> byLower;
    for (uint32_t b = 0; b < n; ++b)
    {
        const std::string name = BoneName(skel, b);
        SkeletonBoneInfo& bi = r.bones[b];
        if (name.empty())
        {
            AddIssue(r, SkeletonIssueKind::EmptyName, static_cast<int32_t>(b), -1, "the bone has no name");
            continue;
        }
        const auto [hit, fresh] = byHash.emplace(BoneNameHash(name.c_str()), b);
        if (!fresh)
        {
            const uint32_t a = hit->second;
            if (BoneName(skel, a) == name)
                AddIssue(r, SkeletonIssueKind::DuplicateName, static_cast<int32_t>(b), static_cast<int32_t>(a),
                         "the same name as bone [" + std::to_string(a) + "]");
            else
                AddIssue(r, SkeletonIssueKind::NameHashCollision, static_cast<int32_t>(b),
                         static_cast<int32_t>(a),
                         "its name hashes like bone [" + std::to_string(a) + "] '" + BoneName(skel, a) +
                         "': a clip channel naming either binds both");
        }
        else
        {
            const auto [low, lowFresh] = byLower.emplace(LowerAscii(name), b);
            if (!lowFresh)
                AddIssue(r, SkeletonIssueKind::NearDuplicateName, static_cast<int32_t>(b),
                         static_cast<int32_t>(low->second),
                         "differs from bone [" + std::to_string(low->second) + "] '" +
                             BoneName(skel, low->second) + "' only in letter case (binding is case-sensitive)");
        }
        if (NamesTwist(name))
        {
            bi.flags |= kSkelBoneTwist;
            ++r.stats.twist;
        }
        if (ChainPhysicsNames::ClassifyBone(name.c_str()) != 0)
        {
            bi.flags |= kSkelBoneChainName;
            ++r.stats.chainName;
        }
        if (ChainPhysicsNames::TriggersAutoAttach(name.c_str()))
        {
            bi.flags |= kSkelBoneChainAttach;
            r.stats.chainAutoAttach = true;
        }
    }

    // ---- 3. transforms: joints, lengths, rest vs bind, mirrored / non-uniform ----
    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
    for (uint32_t b = 0; b < n; ++b)
    {
        const XMVECTOR j = JointOf(skel.bindPose[b]);
        lo = XMVectorMin(lo, j);
        hi = XMVectorMax(hi, j);
    }
    r.stats.modelSize = n ? XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) : 0.f;
    const float mismatchTol = (std::max)(1e-4f, 1e-3f * r.stats.modelSize);

    // FK of the rest locals, row-vector like every pose pass: model = local * parentModel.
    std::vector<XMFLOAT4X4> restModel(n);
    std::vector<uint8_t>    haveModel(n, 0);
    for (uint32_t b = 0; b < n; ++b)
    {
        SkeletonBoneInfo& bi = r.bones[b];
        const XMMATRIX local = XMLoadFloat4x4(&skel.restPoseLocal[b]);

        if (validParent(b))
            bi.length = XMVectorGetX(XMVector3Length(
                XMVectorSubtract(JointOf(skel.bindPose[b]),
                                 JointOf(skel.bindPose[static_cast<uint32_t>(skel.parentIndex[b])]))));

        const int32_t p = skel.parentIndex[b];
        if (p == -1)
        {
            XMStoreFloat4x4(&restModel[b], local);
            haveModel[b] = 1;
        }
        else if (validParent(b) && p < static_cast<int32_t>(b) && haveModel[static_cast<uint32_t>(p)])
        {
            XMStoreFloat4x4(&restModel[b],
                            XMMatrixMultiply(local, XMLoadFloat4x4(&restModel[static_cast<uint32_t>(p)])));
            haveModel[b] = 1;
        }
        if (haveModel[b])
        {
            const float d = XMVectorGetX(XMVector3Length(
                XMVectorSubtract(JointOf(restModel[b]), JointOf(skel.bindPose[b]))));
            if (d > mismatchTol)
                AddIssue(r, SkeletonIssueKind::BindRestMismatch, static_cast<int32_t>(b), -1,
                         "the reference pose puts the joint " + std::to_string(d) +
                             " model units from its bind position");
        }

        const float det = XMVectorGetX(XMMatrixDeterminant(local));
        if (det < 0.f)
        {
            bi.flags |= kSkelBoneMirrored;
            AddIssue(r, SkeletonIssueKind::MirroredRest, static_cast<int32_t>(b), -1,
                     "the rest local is mirrored (negative determinant)");
        }
        const float sx = XMVectorGetX(XMVector3Length(local.r[0]));
        const float sy = XMVectorGetX(XMVector3Length(local.r[1]));
        const float sz = XMVectorGetX(XMVector3Length(local.r[2]));
        const float smin = (std::min)({ sx, sy, sz }), smax = (std::max)({ sx, sy, sz });
        if (smin > 0.f && smax / smin > 1.01f) bi.flags |= kSkelBoneNonUniform;
    }

    // ---- 4. grants: the runtime's own predicate (src >= 0, |ratio| >= 0.001) ----
    for (uint32_t b = 0; b < n; ++b)
    {
        const int32_t src   = skel.grantSource[b];
        const float   ratio = skel.grantRatio[b];
        if (src < 0 || std::fabs(ratio) < 0.001f) continue;
        if (src >= static_cast<int32_t>(n) || src == static_cast<int32_t>(b))
        {
            AddIssue(r, SkeletonIssueKind::BadGrant, static_cast<int32_t>(b), -1,
                     "grantSource " + std::to_string(src) + " is not another bone of this skeleton");
            continue;
        }
        SkeletonBoneInfo& bi = r.bones[b];
        bi.flags |= kSkelBoneGrantTarget;
        bi.grantSource = src;
        bi.grantRatio  = ratio;
        r.bones[static_cast<uint32_t>(src)].flags |= kSkelBoneGrantSource;
        ++r.stats.grantTargets;
    }

    // ---- 5. IK chains ----
    r.stats.ikChains = static_cast<uint32_t>(skel.ikChains.size());
    const auto ikMark = [&](uint32_t bone, uint32_t flag, std::size_t chain, const char* role) {
        if (bone < n)
        {
            r.bones[bone].flags |= flag;
            return;
        }
        AddIssue(r, SkeletonIssueKind::BadIKIndex, -1, -1,
                 "IK chain " + std::to_string(chain) + " names bone " + std::to_string(bone) + " as its " +
                     role + " (" + std::to_string(n) + " bones)");
    };
    for (std::size_t c = 0; c < skel.ikChains.size(); ++c)
    {
        const auto& chain = skel.ikChains[c];
        ikMark(chain.ikBoneIndex, kSkelBoneIKBone, c, "IK bone");
        ikMark(chain.targetBoneIndex, kSkelBoneIKTarget, c, "target");
        for (const auto& link : chain.links) ikMark(link.boneIndex, kSkelBoneIKLink, c, "link");
        r.stats.ikLinks += static_cast<uint32_t>(chain.links.size());
    }

    // ---- 6. humanoid roles ----
    for (uint32_t role = 0; role < kHumanoidBoneCount; ++role)
    {
        const int32_t b = skel.humanoid.boneForRole[role];
        if (b < 0 || b >= static_cast<int32_t>(n)) continue;
        ++r.stats.humanoidMapped;
        SkeletonBoneInfo& bi = r.bones[static_cast<uint32_t>(b)];
        bi.flags |= kSkelBoneHumanoid;
        if (bi.humanoidRole < 0) bi.humanoidRole = static_cast<int16_t>(role);
    }
    r.stats.humanoidValid = skel.humanoid.IsValid();
    if (r.stats.humanoidMapped > 0 && !r.stats.humanoidValid)
    {
        std::string missing;
        for (uint32_t role = 0; role < kHumanoidBoneCount; ++role)
        {
            const auto hb = static_cast<HumanoidBone>(role);
            if (!IsRequiredHumanoidBone(hb) || skel.humanoid.Has(hb)) continue;
            if (!missing.empty()) missing += ", ";
            missing += HumanoidRoleName(hb);
        }
        AddIssue(r, SkeletonIssueKind::HumanoidIncomplete, -1, -1,
                 "required humanoid roles unmapped: " + (missing.empty() ? std::string("(none named)") : missing));
    }

    // ---- 7. skin weights (the .iskel's blend streams) ----
    if (!weights.empty())
    {
        r.stats.weightsKnown = true;
        r.stats.skinStreams  = static_cast<uint32_t>(weights.size());
        std::size_t badWeights = 0;
        uint32_t    firstBad   = 0;
        for (const SkinWeightStream& s : weights)
        {
            if (!s.data) continue;
            r.stats.skinVertices += static_cast<uint32_t>(s.count);
            for (std::size_t v = 0; v < s.count; ++v)
            {
                const BlendVertex& bv = s.data[v];
                uint32_t influences = 0;
                for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k)
                {
                    const uint8_t w = bv.boneWeights[k];
                    if (w == 0) continue;
                    ++influences;
                    const uint32_t idx = bv.boneIndices[k];
                    if (idx >= n)
                    {
                        if (badWeights++ == 0) firstBad = idx;
                        continue;
                    }
                    // A vertex naming one bone twice counts once.
                    bool repeated = false;
                    for (uint32_t e = 0; e < k && !repeated; ++e)
                        repeated = bv.boneWeights[e] != 0 && bv.boneIndices[e] == idx;
                    SkeletonBoneInfo& bi = r.bones[idx];
                    if (!repeated) ++bi.weightedVerts;
                    bi.maxWeight = (std::max)(bi.maxWeight, w / 255.f);
                }
                r.stats.maxInfluences = (std::max)(r.stats.maxInfluences, influences);
            }
        }
        if (badWeights)
            AddIssue(r, SkeletonIssueKind::SkinIndexOutOfRange, -1, -1,
                     std::to_string(badWeights) + " skin weight(s) name a bone this skeleton does not have "
                     "(first: " + std::to_string(firstBad) + "; " + std::to_string(n) + " bones)");
        for (uint32_t b = 0; b < n; ++b)
        {
            SkeletonBoneInfo& bi = r.bones[b];
            if (bi.weightedVerts > 0)
            {
                bi.flags |= kSkelBoneWeighted;
                ++r.stats.weighted;
                continue;
            }
            ++r.stats.unweighted;
            if (bi.flags & kSkelBoneLeaf)
            {
                bi.flags |= kSkelBoneDummyLeaf;
                ++r.stats.dummyLeaves;
            }
        }
        // The ChainPhysics fork shape: a named chain bone whose children are an
        // unweighted leaf AND a real continuation - the strand builder traces one
        // strand per leaf path, so the dummy leaf competes with the visible child.
        for (uint32_t b = 0; b < n; ++b)
        {
            if (!(r.bones[b].flags & kSkelBoneChainName)) continue;
            int32_t dummy = -1;
            bool    real  = false;
            for (const uint32_t c : children[b])
            {
                if (r.bones[c].flags & kSkelBoneDummyLeaf) { if (dummy < 0) dummy = static_cast<int32_t>(c); }
                else if (!(r.bones[c].flags & kSkelBoneLeaf)) real = true;
            }
            if (dummy >= 0 && real)
                AddIssue(r, SkeletonIssueKind::ForkWithDummyLeaf, static_cast<int32_t>(b), dummy,
                         "forks into the unweighted leaf '" + BoneName(skel, static_cast<uint32_t>(dummy)) +
                             "' and a real child: chain physics traces a strand down each");
        }
    }
    return r;
}
ClipFit FitClip(const AnimClipData& clip, const SkeletonAsset& skel)
{
    ClipFit f;
    f.clip     = std::string(clip.name, strnlen(clip.name, sizeof(clip.name)));
    f.channels = static_cast<uint32_t>(clip.channels.size());
    f.bones    = skel.boneCount;
    f.frames   = clip.frameCount;
    f.duration = clip.duration;
    const std::vector<int32_t> binding = clip.ChannelBinding(skel);
    std::vector<bool> used(clip.channels.size(), false);
    for (const int32_t c : binding)
        if (c >= 0)
        {
            ++f.boundBones;
            used[static_cast<std::size_t>(c)] = true;
        }
    std::unordered_map<std::string, int> seen;
    for (std::size_t ci = 0; ci < clip.channels.size(); ++ci)
    {
        const char (&n)[64] = clip.channels[ci].name;
        const std::string name(n, strnlen(n, sizeof(n)));
        if (++seen[name] == 2) f.duplicates.push_back(name);
        if (used[ci]) continue;
        ++f.unmatchedCount;
        if (f.unmatched.size() < 32) f.unmatched.push_back(name);
    }
    return f;
}
}
