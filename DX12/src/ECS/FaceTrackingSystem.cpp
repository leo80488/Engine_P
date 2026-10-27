#include "ECS/FaceTrackingSystem.h"

#include "ECS/AnimationComponents.h"    // SkeletonComponent, kInvalidAnimHandle
#include "ECS/FaceTrackingComponent.h"
#include "ECS/FacialAnimationSystem.h"  // FacialAnimationComponent, Facial::FindMorphSlot
#include "ECS/FacialPresets.h"          // Facial::PresetsOfModel / OwnerModelPath
#include "ECS/IKRigComponent.h"         // a LookAt goal on a tracked bone is warned about
#include "Anim/Core/PoseOps.h"          // RefPose: the rest pose a bake turns
#include "FaceTracking/FaceBinding.h"
#include "FaceTracking/FaceTake.h"
#include "FaceTracking/FaceTakePlayer.h" // a bake reads its take as a replay does
#include "FaceTracking/FaceTrackingService.h"
#include "Physics/ChainPhysicsSystem.h" // ChainPhysicsComponent: the bones it rewrites after us
#include "Resource/AnimationResource.h" // the bake's clips
#include "Resource/AnimationSerializer.h"
#include "Resource/AssetFS.h"
#include "Resource/HumanoidBone.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "System/Log.h"
#include "System/PathUtil.h"            // the bake's .ianim (UTF-8 paths, atomic write)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>

using namespace DirectX;
namespace FT = FaceTracking;
namespace FP = FaceTracking::FacePose;
using FaceTracking::FaceMath::QMul;

namespace
{
std::atomic<uint64_t> s_profileGeneration{1};

constexpr const char* kFaceMapDir = "engine/facemaps/";
constexpr int kMaxBaseDepth = 4;
constexpr float kAlphaEpsilon = 1e-4f; // entries at or below this never reach the fold

inline float Sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float DegToRad(float d) { return d * (FT::FaceMath::kPi / 180.0f); }

// ==== Profiles ====
bool LoadProfile(const std::string& path, FT::FaceMapProfile& out, std::string& err, int depth)
{
    std::string text;
    if (!Resource::AssetFS::Get().ReadFileText(path, text))
    {
        err = "cannot read '" + path + "'";
        return false;
    }
    FT::FaceMapProfile p;
    std::string perr;
    if (!FT::ParseFaceMap(text, p, perr))
    {
        err = path + ": " + perr;
        return false;
    }
    if (!p.basePreset.empty())
    {
        if (depth >= kMaxBaseDepth)
        {
            err = path + ": basePreset chain deeper than " + std::to_string(kMaxBaseDepth) + " (a cycle?)";
            return false;
        }
        FT::FaceMapProfile base;
        if (!LoadProfile(std::string(kFaceMapDir) + p.basePreset + ".ifacemap", base, err, depth + 1)) return false;
        p = FT::MergeFaceMaps(base, p);
    }
    out = std::move(p);
    return true;
}

// "default" (FaceFilter.h's groups), "smooth" (every cutoff halved), "responsive" (doubled),
// "off" (unfiltered: what a recorded take compares against).
FT::FaceFilterSettings FilterPreset(const std::string& name, std::vector<std::string>& warnings)
{
    FT::FaceFilterSettings s;
    auto scale = [&s](float k) {
        for (FT::OneEuroParams& g : s.group) g.minCutoff *= k;
        s.head.minCutoff *= k;
        s.eyes.minCutoff *= k;
    };
    if (name.empty() || name == "default") return s;
    if (name == "smooth") scale(0.5f);
    else if (name == "responsive") scale(2.0f);
    else if (name == "off")
    {
        for (FT::OneEuroParams& g : s.group) g.enabled = false;
        s.head.enabled = false;
        s.eyes.enabled = false;
    }
    else
        warnings.push_back("filters.preset '" + name + "' is not default / smooth / responsive / off: default used");
    return s;
}

// ==== Bones ====
// Name table, tried after the HumanoidProfile: PMX's Japanese names, VRoid's J_Bip,
// Mixamo, plain English. PMX's both-eyes bone (ryoume) is never listed: it grants its
// rotation to both eyes, so writing it as well would count the eyes twice. The
// Japanese names are UTF-8 BYTE ESCAPES (as FacialAnimationComponent.h's blinkMorph
// is): matching is byte-exact, and re-saving this file in another encoding must not
// silently change them.
struct RoleNames
{
    HumanoidBone role;
    const char* names[6];
};
constexpr RoleNames kRoleNames[] = {
    {HumanoidBone::Spine, {"\xe4\xb8\x8a\xe5\x8d\x8a\xe8\xba\xab", "Spine", "spine", "J_Bip_C_Spine", "mixamorig:Spine", nullptr}}, // jouhanshin (upper body)
    {HumanoidBone::Chest, {"\xe4\xb8\x8a\xe5\x8d\x8a\xe8\xba\xab" "2", "Chest", "chest", "J_Bip_C_Chest", "mixamorig:Spine1", nullptr}}, // jouhanshin2
    {HumanoidBone::UpperChest, {"\xe4\xb8\x8a\xe5\x8d\x8a\xe8\xba\xab" "3", "UpperChest", "upperChest", "J_Bip_C_UpperChest", "mixamorig:Spine2", nullptr}}, // jouhanshin3
    {HumanoidBone::Neck, {"\xe9\xa6\x96", "Neck", "neck", "J_Bip_C_Neck", "mixamorig:Neck", nullptr}}, // kubi (neck)
    {HumanoidBone::Head, {"\xe9\xa0\xad", "Head", "head", "J_Bip_C_Head", "mixamorig:Head", nullptr}}, // atama (head)
    {HumanoidBone::LeftEye, {"\xe5\xb7\xa6\xe7\x9b\xae", "LeftEye", "leftEye", "J_Adj_L_FaceEye", "mixamorig:LeftEye", "Eye_L"}}, // hidari me (left eye)
    {HumanoidBone::RightEye, {"\xe5\x8f\xb3\xe7\x9b\xae", "RightEye", "rightEye", "J_Adj_R_FaceEye", "mixamorig:RightEye", "Eye_R"}}, // migi me (right eye)
    {HumanoidBone::Jaw, {"\xe3\x81\x82\xe3\x81\x94", "Jaw", "jaw", "J_Bip_C_Jaw", nullptr, nullptr}}, // ago (jaw)
};

int FindBone(const SkeletonAsset& skel, std::string_view name)
{
    if (name.empty()) return -1;
    for (uint32_t b = 0; b < skel.boneCount; ++b)
        if (name == std::string_view(skel.boneNames[b])) return static_cast<int>(b);
    return -1;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}

// A role name in any case ("Neck", "neck", "upperChest"), against HumanoidRoleName.
bool ParseRole(std::string_view name, HumanoidBone& out)
{
    for (uint32_t r = 0; r < kHumanoidBoneCount; ++r)
        if (EqualsNoCase(name, HumanoidRoleName(static_cast<HumanoidBone>(r))))
        {
            out = static_cast<HumanoidBone>(r);
            return true;
        }
    return false;
}

int ResolveRole(const SkeletonAsset& skel, HumanoidBone role)
{
    const int32_t viaProfile = skel.humanoid.Bone(role);
    if (viaProfile >= 0 && static_cast<uint32_t>(viaProfile) < skel.boneCount) return viaProfile;
    for (const RoleNames& rn : kRoleNames)
    {
        if (rn.role != role) continue;
        for (const char* n : rn.names)
            if (n)
                if (const int b = FindBone(skel, n); b >= 0) return b;
    }
    return -1;
}

// A chain entry: its role (profile, then the name table), else its explicit bone name.
int ResolveEntry(const SkeletonAsset& skel, const std::string& roleName, const std::string& boneName)
{
    HumanoidBone role;
    if (!roleName.empty() && ParseRole(roleName, role))
        if (const int b = ResolveRole(skel, role); b >= 0) return b;
    return FindBone(skel, boneName);
}

bool IsAncestorOrSelf(const SkeletonAsset& skel, int ancestor, int bone)
{
    for (int b = bone; b >= 0; b = skel.parentIndex[b])
    {
        if (b == ancestor) return true;
        if (skel.parentIndex[b] >= b) break; // parents come first; anything else is malformed
    }
    return false;
}

XMVECTOR RestRotation(const SkeletonAsset& skel, int b)
{
    XMVECTOR s, r, t;
    return XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&skel.restPoseLocal[b])) ? XMQuaternionNormalize(r)
                                                                                     : XMQuaternionIdentity();
}

// The rest pose in model space: restPoseLocal up the parent chain (row vectors: local, then parent).
XMMATRIX RestModelMatrix(const SkeletonAsset& skel, int b)
{
    XMMATRIX m = XMMatrixIdentity();
    for (int i = b; i >= 0; i = skel.parentIndex[i])
    {
        m = XMMatrixMultiply(m, XMLoadFloat4x4(&skel.restPoseLocal[i]));
        if (skel.parentIndex[i] >= i) break;
    }
    return m;
}

XMVECTOR RestModelRotation(const SkeletonAsset& skel, int b)
{
    XMVECTOR q = XMQuaternionIdentity();
    for (int i = b; i >= 0; i = skel.parentIndex[i])
    {
        q = QMul(q, RestRotation(skel, i));
        if (skel.parentIndex[i] >= i) break;
    }
    return XMQuaternionNormalize(q);
}

// The bone's model-space rotation in the pose as it is now (scale-free: bone rotations only).
XMVECTOR ModelRotation(const AnimationSystem::LocalPose* poses, const SkeletonAsset& skel, int b)
{
    XMVECTOR q = XMQuaternionIdentity();
    for (int i = b; i >= 0; i = skel.parentIndex[i])
    {
        q = QMul(q, XMQuaternionNormalize(XMLoadFloat4(&poses[i].rot)));
        if (skel.parentIndex[i] >= i) break;
    }
    return XMQuaternionNormalize(q);
}

XMFLOAT3 Translation(FXMMATRIX m)
{
    XMFLOAT3 t;
    XMStoreFloat3(&t, m.r[3]);
    return t;
}

void Warn(FT::BindingRuntime& rt, std::string w) { rt.warnings.push_back(std::move(w)); }

// Bone resolution for the profile just compiled, against this skeleton. Cached on the
// runtime: redone only when the compile or the skeleton changes.
void ResolveBones(World& world, Entity e, SkeletonAsset& skel, uint32_t assetIndex, FT::BindingRuntime& rt)
{
    rt.bonesTried = true;
    rt.bonesFor = assetIndex;
    rt.bonesStamp = rt.compileStamp;
    rt.chainCount = 0;
    rt.chainRootParent = rt.headBone = rt.leftEye = rt.rightEye = rt.jawBone = -1;
    rt.basis = rt.rootParentRest = rt.headRest = {0, 0, 0, 1};
    rt.basisFromEyes = false;
    if (!rt.compiled || !rt.profile.hasBones) return;
    const FT::BoneMap& bm = rt.profile.bones;

    // Serial here (the animation chain); AnimationSystem and IKRig build it the same way.
    if (!skel.humanoid.IsValid() && !skel.humanoid.userEdited) Resource::HumanoidProfileBuilder::Build(skel);

    // ---- the head chain: role, fallbackRole, then the weights renormalized ----
    const size_t n = std::min(bm.chain.size(), static_cast<size_t>(FT::kMaxChainBones));
    if (bm.chain.size() > n) Warn(rt, "bones.chain: only the first " + std::to_string(n) + " entries are used");
    std::array<float, FT::kMaxChainBones> weight{};
    std::array<bool, FT::kMaxChainBones> resolved{};
    std::array<int, FT::kMaxChainBones> bone{};
    for (size_t i = 0; i < n; ++i)
    {
        const FT::ChainBone& c = bm.chain[i];
        int b = ResolveEntry(skel, c.role, c.bone);
        if (b < 0 && !c.fallbackRole.empty())
        {
            b = ResolveEntry(skel, c.fallbackRole, std::string());
            if (b >= 0)
                Warn(rt, "bones.chain: role '" + c.role + "' is missing; fallbackRole '" + c.fallbackRole + "' used (" +
                             skel.boneNames[b] + ")");
        }
        // Two entries on one bone (a fallback that lands on a bone already in the chain) add up.
        for (size_t j = 0; j < i && b >= 0; ++j)
            if (resolved[j] && bone[j] == b)
            {
                weight[j] += std::max(0.0f, c.weight);
                b = -2;
            }
        bone[i] = b;
        resolved[i] = b >= 0;
        weight[i] = c.weight;
        if (b == -1)
            Warn(rt, "bones.chain: '" + (c.role.empty() ? c.bone : c.role) + "' resolves to no bone; its weight goes to the others");
    }
    if (FP::NormalizeChainWeights(weight, resolved, n))
    {
        std::array<std::pair<int, float>, FT::kMaxChainBones> picked{};
        int count = 0;
        for (size_t i = 0; i < n; ++i)
            if (resolved[i] && weight[i] > 0.0f) picked[count++] = {bone[i], weight[i]};
        std::sort(picked.begin(), picked.begin() + count, [](const auto& a, const auto& b) { return a.first < b.first; });
        for (int k = 0; k < count; ++k)
        {
            rt.chainBone[k] = picked[k].first;
            rt.chainWeight[k] = picked[k].second;
        }
        rt.chainCount = count;
        const int tip = rt.chainBone[count - 1];
        for (int k = 0; k + 1 < count; ++k)
            if (!IsAncestorOrSelf(skel, rt.chainBone[k], tip))
                Warn(rt, std::string("bones.chain: ") + skel.boneNames[rt.chainBone[k]] + " is not above " + skel.boneNames[tip] +
                             ": the head will turn less than tracked");
        rt.chainRootParent = skel.parentIndex[rt.chainBone[0]];
    }
    else if (n > 0)
        Warn(rt, "bones.chain: no entry resolves to a bone - the head is not tracked");

    // ---- head, eyes, jaw ----
    rt.headBone = ResolveRole(skel, HumanoidBone::Head);
    if (rt.headBone < 0 && rt.chainCount > 0) rt.headBone = rt.chainBone[rt.chainCount - 1];
    const int eyeL = bm.leftEye.empty() ? ResolveRole(skel, HumanoidBone::LeftEye) : FindBone(skel, bm.leftEye);
    const int eyeR = bm.rightEye.empty() ? ResolveRole(skel, HumanoidBone::RightEye) : FindBone(skel, bm.rightEye);
    if (bm.eyes == FT::EyesMode::Bones)
    {
        rt.leftEye = eyeL;
        rt.rightEye = eyeR;
        if (eyeL < 0 || eyeR < 0)
            Warn(rt, std::string("bones.eyes: ") + (eyeL < 0 ? "the left" : "the right") + " eye bone is missing");
        if (rt.headBone < 0 && (eyeL >= 0 || eyeR >= 0))
        {
            Warn(rt, "bones.eyes: no head bone to follow - the eyes are not tracked");
            rt.leftEye = rt.rightEye = -1;
        }
    }
    if (bm.jaw == FT::JawMode::Bone)
    {
        rt.jawBone = bm.jawBone.empty() ? ResolveRole(skel, HumanoidBone::Jaw) : FindBone(skel, bm.jawBone);
        if (rt.jawBone < 0) Warn(rt, "bones.jaw: no jaw bone - the jaw is not tracked");
        else if (rt.headBone < 0)
        {
            Warn(rt, "bones.jaw: no head bone to follow - the jaw is not tracked");
            rt.jawBone = -1;
        }
    }

    // ---- the face basis: from the eyes when it can, else the head bone's named local axes ----
    FP::FaceBasis basis;
    bool haveBasis = false;
    if (bm.basisAuto && eyeL >= 0 && eyeR >= 0 && rt.headBone >= 0)
    {
        const int neck = ResolveRole(skel, HumanoidBone::Neck);
        const int below = (neck >= 0 && neck != rt.headBone) ? neck : skel.parentIndex[rt.headBone];
        if (below >= 0)
            haveBasis = FP::BasisFromEyes(Translation(RestModelMatrix(skel, eyeL)), Translation(RestModelMatrix(skel, eyeR)),
                                          Translation(RestModelMatrix(skel, rt.headBone)), Translation(RestModelMatrix(skel, below)),
                                          basis);
        rt.basisFromEyes = haveBasis;
    }
    if (!haveBasis && rt.headBone >= 0)
    {
        haveBasis = FP::BasisFromAxes(bm.forward, bm.up, RestModelRotation(skel, rt.headBone), basis);
        if (!haveBasis) Warn(rt, "bones.faceBasis: forward '" + bm.forward + "' / up '" + bm.up + "' are not two different axes");
    }
    if (!haveBasis && (rt.chainCount > 0 || rt.leftEye >= 0 || rt.rightEye >= 0 || rt.jawBone >= 0))
        Warn(rt, "bones.faceBasis: none could be derived - the model's own axes are used");
    rt.basis = FP::Store(haveBasis ? FP::BasisRotation(basis) : XMQuaternionIdentity());
    if (rt.chainRootParent >= 0) rt.rootParentRest = FP::Store(RestModelRotation(skel, rt.chainRootParent));
    if (rt.headBone >= 0) rt.headRest = FP::Store(RestModelRotation(skel, rt.headBone));

    // ---- what rewrites these bones after us: warned, never fixed ----
    int written[FT::kMaxChainBones + 3];
    int nw = 0;
    for (int k = 0; k < rt.chainCount; ++k) written[nw++] = rt.chainBone[k];
    for (int b : {rt.leftEye, rt.rightEye, rt.jawBone})
        if (b >= 0) written[nw++] = b;
    if (const auto* chain = world.GetComponent<ChainPhysicsComponent>(e))
    {
        for (const ChainGroupDef& g : chain->groups)
        {
            const int root = FindBone(skel, g.rootBone);
            for (int k = 0; k < nw && root >= 0; ++k)
                if (IsAncestorOrSelf(skel, root, written[k]))
                    LOG_WARNING("[FACETRACK] entity %u: %s is simulated by chain physics group '%s' - chain physics "
                                "rewrites it after face tracking", static_cast<unsigned>(e), skel.boneNames[written[k]], g.name);
        }
        for (const BoneRestPose& rp : chain->restPose)
            for (int k = 0; k < nw; ++k)
                if (std::strcmp(rp.boneName, skel.boneNames[written[k]]) == 0)
                    LOG_WARNING("[FACETRACK] entity %u: %s has a chain physics rest pose - chain physics rewrites it after "
                                "face tracking", static_cast<unsigned>(e), rp.boneName);
    }
    if (const auto* ik = world.GetComponent<IKRigComponent>(e))
        for (const IKGoalDef& g : ik->goals)
        {
            if (!g.enabled || g.solver != IKSolverKind::LookAt) continue;
            for (int k = 0; k < nw; ++k)
                for (const std::string* name : {&g.endBone, &g.ikBone, &g.rootBone})
                    if (*name == skel.boneNames[written[k]])
                        LOG_WARNING("[FACETRACK] entity %u: an IKRig LookAt goal turns %s too - the two fight over it",
                                    static_cast<unsigned>(e), skel.boneNames[written[k]]);
        }
}

// ==== Compile ====
void Compile(World& world, Entity e, const FaceTrackingComponent& ft, SkeletonAsset& skel, uint32_t assetIndex,
             FT::BindingRuntime& rt, uint64_t generation, const FT::FaceSourceChannels* channelTable = nullptr)
{
    rt.profileRequested = ft.profile;
    rt.profilePath = ft.profile.empty() ? FaceTrackingSystem::AutoProfilePath(skel) : ft.profile;
    rt.profileGeneration = generation;
    rt.skeletonAsset = assetIndex;
    rt.compiled = false;
    rt.compileFailed = false;
    rt.compileError.clear();
    rt.warnings.clear();
    ++rt.compileStamp;

    FT::FaceMapProfile profile;
    std::string err;
    FT::FaceSourceChannels channels; // a source not open yet: the ARKit names only (recompiled once it has extras)
    if (channelTable) channels = *channelTable; // a bake: the take's own table
    else FT::Service::Get().Channels(ft.source, channels);
    rt.sourceExtras = channels.Extras().size();
    FT::CompiledFaceMap map;
    bool ok = LoadProfile(rt.profilePath, profile, err, 0);
    if (ok)
    {
        const std::string modelPath = Facial::OwnerModelPath(world, e);
        std::shared_ptr<const Facial::PresetTable> presets; // fetched by the first preset: target
        FT::FaceMapBindHooks hooks;
        hooks.findMorphSlot = [&skel](std::string_view name) {
            const std::string n(name);
            return static_cast<int>(Facial::FindMorphSlot(skel, n.c_str()));
        };
        hooks.expandPreset = [&](std::string_view name, std::vector<std::pair<std::string, float>>& out) {
            if (!presets) presets = Facial::PresetsOfModel(modelPath);
            const auto it = presets->find(std::string(name));
            if (it == presets->end()) return false;
            out = it->second;
            return true;
        };
        ok = FT::CompileFaceMap(profile, channels, hooks, map, err);
    }
    if (!ok)
    {
        rt.compileFailed = true;
        rt.compileError = err;
        rt.map = FT::CompiledFaceMap{};
        rt.profile = FT::FaceMapProfile{};
        LOG_WARNING("[FACETRACK] entity %u: mapping '%s' refused: %s", static_cast<unsigned>(e), rt.profilePath.c_str(), err.c_str());
        ResolveBones(world, e, skel, assetIndex, rt);
        return;
    }
    rt.filters = FilterPreset(profile.filterPreset, rt.warnings);
    rt.warnings.insert(rt.warnings.end(), map.warnings.begin(), map.warnings.end());
    rt.profile = std::move(profile);
    rt.map = std::move(map);
    rt.compiled = true;
    ResolveBones(world, e, skel, assetIndex, rt);

    size_t slots = 0;
    for (const FT::CompiledFaceMap::Target& t : rt.map.targets) slots += t.out.size();
    LOG_INFO("[FACETRACK] bind entity=%u source=%s profile=%s targets=%zu slots=%zu vowels=%d chain=%d eyes=%d jaw=%d "
             "basis=%s warnings=%zu",
             static_cast<unsigned>(e), ft.source.c_str(), rt.profilePath.c_str(), rt.map.targets.size(), slots,
             rt.map.vowelsEnabled ? 1 : 0, rt.chainCount, (rt.leftEye >= 0) + (rt.rightEye >= 0), rt.jawBone >= 0 ? 1 : 0,
             rt.basisFromEyes ? "eyes" : "axes", rt.warnings.size());
    for (const std::string& w : rt.warnings)
        LOG_INFO("[FACETRACK] bind entity=%u: %s", static_cast<unsigned>(e), w.c_str());
}

void ClearOutputs(FT::BindingRuntime& rt)
{
    rt.trackedCount = 0;
    rt.morphAlpha = 0.0f;
    rt.blinkHeld = false;
    rt.authorityValue = 0.0f;
    rt.haveHead = rt.haveEyes = rt.haveJaw = false;
}

// For one new frame: calibrate, mirror, filter into the processed history - found
// frames only (the last found face is what is held).
void ConsumeFrame(FT::BindingRuntime& rt, bool mirror, FT::FaceFrame& f, int64_t& foundUs)
{
    ++rt.framesConsumed;
    rt.newestFound = f.faceFound != 0;
    if (!f.faceFound) return;
    FP::Calibrate(f, rt.calibration);
    if (mirror != ((f.flags & FT::kFaceMirrored) != 0)) FP::Mirror(f);
    rt.filter.Apply(f, rt.filters);
    if (rt.processed.Append(f)) foundUs = f.sourceTimeUs;
}

// Evaluates this frame's sample (rt.sampled) at authority a: the morphs (onto fa's
// Tracked layer when there is one; also into rt.slotWeights when wantWeights - a
// bake has no fold), auto-blink's hold, and the bone targets PoseBinding turns.
void EvaluateBinding(FaceTrackingComponent& ft, FT::BindingRuntime& rt, const SkeletonAsset& skel,
                     FacialAnimationComponent* fa, float a, bool wasBlinkHeld, bool wantWeights)
{
    static const int kBlinkL = FT::FindArkitChannel("eyeBlinkLeft");
    static const int kBlinkR = FT::FindArkitChannel("eyeBlinkRight");
    static const int kJawOpen = FT::FindArkitChannel("jawOpen");
    static const int kLookIn[2] = {FT::FindArkitChannel("eyeLookInLeft"), FT::FindArkitChannel("eyeLookInRight")};
    static const int kLookOut[2] = {FT::FindArkitChannel("eyeLookOutLeft"), FT::FindArkitChannel("eyeLookOutRight")};
    static const int kLookUp[2] = {FT::FindArkitChannel("eyeLookUpLeft"), FT::FindArkitChannel("eyeLookUpRight")};
    static const int kLookDown[2] = {FT::FindArkitChannel("eyeLookDownLeft"), FT::FindArkitChannel("eyeLookDownRight")};

    // ---- morphs -> the Tracked layer ----
    const float alpha = a * Sat(ft.weightMorph);
    if ((fa || wantWeights) && alpha > kAlphaEpsilon)
    {
        rt.map.Evaluate(rt.sampled.values, rt.sampled.present, rt.scratch, rt.slotWeights);
        rt.morphAlpha = alpha;
        if (fa)
        {
            for (const auto& [slot, w] : rt.slotWeights)
                if (slot >= 0 && static_cast<uint32_t>(slot) < skel.morphTargetCount)
                    fa->tracked.push_back({static_cast<uint16_t>(slot), Sat(w), alpha});
            rt.trackedCount = static_cast<uint32_t>(fa->tracked.size());
        }
    }
    // Auto-blink pauses while the tracked lids own the eyes, with hysteresis.
    const bool lids = rt.sampled.Has(kBlinkL) || rt.sampled.Has(kBlinkR);
    const bool hold = ft.suppressAutoBlink && lids && fa && (wasBlinkHeld ? alpha > 0.45f : alpha >= 0.55f);
    rt.blinkHeld = hold;
    if (fa) fa->trackingHoldsBlink = hold;

    if (!rt.profile.hasBones) return;
    const FT::BoneMap& bm = rt.profile.bones;

    // ---- the head: shaped, then shared by the authority; HeadTrack gives way ----
    const float aHead = a * Sat(ft.weightHead);
    if ((rt.sampled.flags & FT::kFaceHasHead) && rt.chainCount > 0 && aHead > 0.0f)
    {
        FP::HeadShape shape;
        shape.gainYaw = bm.gainYaw;
        shape.gainPitch = bm.gainPitch;
        shape.gainRoll = bm.gainRoll;
        shape.limitYawRad = DegToRad(bm.limitYawDeg);
        shape.limitPitchRad = DegToRad(bm.limitPitchDeg);
        shape.limitRollRad = DegToRad(bm.limitRollDeg);
        const XMVECTOR q = FP::ShapeHead(XMQuaternionNormalize(FP::Load(rt.sampled.headRot)), shape);
        rt.headTarget = FP::Store(FP::Share(q, aHead));
        rt.haveHead = true;
        if (ft.headTrackHandoff) ft.headAuthority = aHead;
    }

    // ---- the eyes: the source's angles, else derived from eyeLook* ----
    const float aEyes = a * Sat(ft.weightEyes);
    if (bm.eyes == FT::EyesMode::Bones && (rt.leftEye >= 0 || rt.rightEye >= 0) && aEyes > 0.0f)
    {
        const float maxYaw = DegToRad(bm.eyeMaxYawDeg), maxPitch = DegToRad(bm.eyeMaxPitchDeg);
        float yp[2][2] = {};
        bool have = false;
        if (rt.sampled.flags & FT::kFaceHasEyes)
        {
            for (int side = 0; side < 2; ++side)
            {
                yp[side][0] = std::clamp(rt.sampled.eyeYawPitch[side][0], -maxYaw, maxYaw);
                yp[side][1] = std::clamp(rt.sampled.eyeYawPitch[side][1], -maxPitch, maxPitch);
            }
            have = true;
        }
        else
        {
            auto ch = [&](int c) { return rt.sampled.Has(c) ? Sat(rt.sampled.values[c]) : 0.0f; };
            for (int side = 0; side < 2; ++side)
                have = have || rt.sampled.Has(kLookIn[side]) || rt.sampled.Has(kLookOut[side]) ||
                       rt.sampled.Has(kLookUp[side]) || rt.sampled.Has(kLookDown[side]);
            // Towards the subject's right: the left eye looks in, the right eye looks out.
            yp[0][0] = (ch(kLookIn[0]) - ch(kLookOut[0])) * maxYaw;
            yp[1][0] = (ch(kLookOut[1]) - ch(kLookIn[1])) * maxYaw;
            for (int side = 0; side < 2; ++side) yp[side][1] = (ch(kLookUp[side]) - ch(kLookDown[side])) * maxPitch;
        }
        if (have)
        {
            for (int side = 0; side < 2; ++side)
            {
                rt.eyeTarget[side] = FP::Store(FT::FaceMath::ComposeEye(yp[side][0] * aEyes, yp[side][1] * aEyes));
                rt.eyeYawPitchDeg[side][0] = yp[side][0] * aEyes * (180.0f / FT::FaceMath::kPi);
                rt.eyeYawPitchDeg[side][1] = yp[side][1] * aEyes * (180.0f / FT::FaceMath::kPi);
            }
            rt.haveEyes = true;
        }
    }

    // ---- the jaw: jawOpen x maxDeg about R, with the morphs' share ----
    if (bm.jaw == FT::JawMode::Bone && rt.jawBone >= 0 && rt.sampled.Has(kJawOpen) && alpha > 0.0f)
    {
        rt.jawAngle = Sat(rt.sampled.values[kJawOpen]) * DegToRad(bm.jawMaxDeg) * alpha;
        rt.haveJaw = rt.jawAngle != 0.0f;
    }
}

// Pose step for one binding: a model-space delta D onto each bone, in its parent's
// space as it is now (with the earlier chain bones' deltas in it):
// local' = local x (Pw D Pw^-1). Each bone is offered to `journal` before it is
// written; a bone it refuses is left alone.
template <class Journal>
void PoseBinding(const SkeletonAsset& skel, const FT::BindingRuntime& rt, AnimationSystem::LocalPose* poses, Journal&& journal)
{
    auto write = [&](int bone, FXMVECTOR dModel) {
        if (bone < 0 || static_cast<uint32_t>(bone) >= skel.boneCount) return;
        if (!journal(bone)) return;
        const int p = skel.parentIndex[bone];
        const XMVECTOR pw = p >= 0 ? ModelRotation(poses, skel, p) : XMQuaternionIdentity();
        const XMVECTOR local = XMQuaternionNormalize(QMul(XMLoadFloat4(&poses[bone].rot), FP::ParentSpace(dModel, pw)));
        XMStoreFloat4(&poses[bone].rot, local);
    };

    const XMVECTOR qB = FP::Load(rt.basis);
    if (rt.haveHead && rt.chainCount > 0)
    {
        // The basis follows the chain root's parent as animated: a bowing character still
        // turns its head about its neck's up of this frame.
        const XMVECTOR rNow = rt.chainRootParent >= 0 ? ModelRotation(poses, skel, rt.chainRootParent) : XMQuaternionIdentity();
        const XMVECTOR qBt = XMQuaternionNormalize(QMul(qB, QMul(FP::Inv(FP::Load(rt.rootParentRest)), rNow)));
        const XMVECTOR qT = FP::Load(rt.headTarget);
        for (int k = 0; k < rt.chainCount; ++k)
            write(rt.chainBone[k], FP::FaceToModel(FP::Share(qT, rt.chainWeight[k]), qBt));
    }
    if ((rt.haveEyes || rt.haveJaw) && rt.headBone >= 0)
    {
        // Eyes and jaw turn in the head's frame as it is now, the tracked turn included.
        const XMVECTOR headNow = ModelRotation(poses, skel, rt.headBone);
        const XMVECTOR qBh = XMQuaternionNormalize(QMul(qB, QMul(FP::Inv(FP::Load(rt.headRest)), headNow)));
        if (rt.haveEyes)
        {
            write(rt.leftEye, FP::FaceToModel(FP::Load(rt.eyeTarget[0]), qBh));
            write(rt.rightEye, FP::FaceToModel(FP::Load(rt.eyeTarget[1]), qBh));
        }
        if (rt.haveJaw) write(rt.jawBone, FP::FaceToModel(FT::FaceMath::JawOpen(rt.jawAngle), qBh));
    }
}

} // namespace

bool FaceTrackingSystem::LoadProfileFile(const std::string& path, FT::FaceMapProfile& out, std::string& error)
{
    return LoadProfile(path, out, error, 0);
}

void FaceTrackingSystem::InvalidateProfiles() { s_profileGeneration.fetch_add(1, std::memory_order_relaxed); }
uint64_t FaceTrackingSystem::ProfileGeneration() { return s_profileGeneration.load(std::memory_order_relaxed); }

std::string FaceTrackingSystem::AutoProfilePath(const SkeletonAsset& skel)
{
    const char* preset = "arkit";
    if (Facial::FindMorphSlot(skel, "Fcl_EYE_Close") >= 0 || Facial::FindMorphSlot(skel, "Fcl_EYE_Close_L") >= 0)
        preset = "vroid";
    else if (Facial::FindMorphSlot(skel, "\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d") >= 0 ||                // mabataki (blink)
             Facial::FindMorphSlot(skel, "\xe3\x81\x82") >= 0)                                       // a (the vowel morph)
        preset = "mmd_jp";
    return std::string(kFaceMapDir) + preset + ".ifacemap";
}

void FaceTrackingSystem::Resolve(World& world, float dt, const std::unordered_set<Entity>* activeSet)
{
    FT::Service& svc = FT::Service::Get();
    svc.Pump(dt);

    auto* pool = world.GetPool<FaceTrackingComponent>();
    if (!pool || pool->Size() == 0) return;
    auto* pSkel = world.GetPool<SkeletonComponent>();
    auto* pFA = world.GetPool<FacialAnimationComponent>();
    const uint64_t generation = s_profileGeneration.load(std::memory_order_relaxed);

    const auto& ents = pool->Entities();
    auto& data = pool->Data();
    for (size_t i = 0; i < data.size(); ++i)
    {
        const Entity e = ents[i];
        FaceTrackingComponent& ft = data[i];
        FacialAnimationComponent* fa = pFA ? pFA->Get(e) : nullptr;
        ft.headAuthority = 0.0f;
        if (fa)
        {
            fa->tracked.clear();
            fa->trackingHoldsBlink = false;
        }

        const SkeletonComponent* skc = pSkel ? pSkel->Get(e) : nullptr;
        if (!skc)
        {
            static bool s_warnedOwner = false;
            if (!s_warnedOwner)
            {
                s_warnedOwner = true;
                LOG_WARNING("[FACETRACK] FaceTrackingComponent on entity %u has no SkeletonComponent - it does nothing. "
                            "Attach it to the skeleton entity (FaceTracking.Attach finds it).", static_cast<unsigned>(e));
            }
        }
        if (!m_enabled || !ft.enabled || !svc.Enabled() || !skc)
        {
            // Off: nothing reaches the face, and the authority starts from 0 again - a binding
            // re-enabled later fades in, it does not land in one frame from where it stood.
            if (ft.rt)
            {
                ClearOutputs(*ft.rt);
                ft.rt->authority.Reset();
            }
            continue;
        }
        if (skc->assetIndex == kInvalidAnimHandle || skc->assetIndex >= m_skeletons.Count()) continue; // still streaming
        SkeletonAsset& skel = m_skeletons.Get(skc->assetIndex);

        const uint32_t gen = world.GetGeneration(e);
        if (!ft.rt || ft.rt->owner != e || ft.rt->ownerGeneration != gen)
        {
            ft.rt = std::make_shared<FT::BindingRuntime>();
            ft.rt->owner = e;
            ft.rt->ownerGeneration = gen;
        }
        FT::BindingRuntime& rt = *ft.rt;

        // A new source or mirror: the processed frames start over from the source's history
        // (its last 2 s), so the face is there on the first frame.
        if (rt.sourceName != ft.source || rt.mirror != ft.mirror)
        {
            if (rt.sourceName != ft.source)
            {
                rt.authority.Reset();
                rt.compiled = rt.compileFailed = false;
                rt.sourceMissing = true;
            }
            rt.sourceName = ft.source;
            rt.mirror = ft.mirror;
            rt.cursor = {};
            rt.filter.Reset();
            rt.processed.Clear();
            rt.newestFound = false;
        }

        FT::SourceStatus st;
        const bool haveSource = svc.Status(ft.source, st);
        if (rt.profileRequested != ft.profile || rt.profileGeneration != generation || rt.skeletonAsset != skc->assetIndex ||
            (haveSource && st.extras > rt.sourceExtras) || (!rt.compiled && !rt.compileFailed))
            Compile(world, e, ft, skel, skc->assetIndex, rt, generation);
        else if (rt.bonesFor != skc->assetIndex || rt.bonesStamp != rt.compileStamp)
            ResolveBones(world, e, skel, skc->assetIndex, rt);

        // ---- the new frames: calibrate, mirror, filter ----
        int64_t foundUs = -1;
        const int64_t clockBefore = rt.nowUs;
        if (haveSource)
        {
            // A source closed and reopened under the same name is a new epoch: it restarts at
            // seq 1 and clock 0, so the frames start over - and so does the authority, whose
            // times belong to the old clock (a last-found time from it would hold the face
            // "found" until the new clock caught up with it).
            // The source's calibration changed (captured, reset, loaded): the frames start over
            // from its history under the new one, so the face moves to it at once.
            FP::Calibration cal;
            uint64_t calGeneration = 0;
            if (!svc.GetCalibration(ft.source, cal, calGeneration)) cal = FP::Calibration{};
            if (calGeneration != rt.calibrationGeneration)
            {
                rt.calibration = cal;
                rt.calibrationGeneration = calGeneration;
                rt.cursor = {};
                rt.filter.Reset();
                rt.processed.Clear();
                rt.newestFound = false;
            }
            if (rt.sourceMissing || st.epoch != rt.sourceEpoch || st.newestSeq < rt.cursor.lastSeq)
            {
                rt.cursor = {};
                rt.filter.Reset();
                rt.processed.Clear();
                rt.newestFound = false;
                if (st.epoch != rt.sourceEpoch) rt.authority.Reset();
                rt.sourceEpoch = st.epoch;
            }
            rt.sourceMissing = false;
            m_frames.clear();
            svc.ReadNew(ft.source, rt.cursor, m_frames);
            for (FT::FaceFrame& f : m_frames) ConsumeFrame(rt, ft.mirror, f, foundUs);
            rt.processed.Trim();
            rt.nowUs = st.clockUs;
        }
        else
        {
            if (!rt.sourceMissing)
                LOG_INFO("[FACETRACK] entity %u: source '%s' is gone - the face fades out", static_cast<unsigned>(e), ft.source.c_str());
            rt.sourceMissing = true;
            rt.newestFound = false;
            if (dt > 0.0f) rt.nowUs += static_cast<int64_t>(std::llround(static_cast<double>(dt) * 1e6));
        }

        // ---- authority: a paused clock snaps instead of fading ----
        // A take's or a script's clock stops in Edit mode (dt = 0): the face lands at once, as
        // HeadTrack's does. A live source runs on the wall clock, so it keeps fading.
        FT::FaceAuthorityParams ap{std::max(0.0f, ft.holdSec), std::max(0.0f, ft.fadeOutSec), std::max(0.0f, ft.fadeInSec)};
        if (rt.nowUs == clockBefore) ap.fadeOutSec = ap.fadeInSec = 0.0f;
        if (haveSource || dt > 0.0f) rt.authority.Update(rt.nowUs, foundUs, rt.newestFound, ap);
        else rt.authority.Reset();

        rt.sampleState = rt.processed.Sample(rt.nowUs, rt.sampled);
        const float a = rt.authority.Value();
        const bool wasBlinkHeld = rt.blinkHeld;
        ClearOutputs(rt);
        rt.authorityValue = a;
        const bool haveFace = rt.compiled && rt.sampleState != FT::SampleState::Empty && a > 0.0f;
        const bool active = !activeSet || activeSet->find(e) != activeSet->end();
        if (!haveFace || !active) continue;

        EvaluateBinding(ft, rt, skel, fa, a, wasBlinkHeld, /*wantWeights=*/false);
    }
}

void FaceTrackingSystem::ApplyPose(World& world, const std::unordered_set<Entity>* activeSet)
{
    if (!m_enabled) return;
    auto* pool = world.GetPool<FaceTrackingComponent>();
    if (!pool || pool->Size() == 0) return;
    auto* pSkel = world.GetPool<SkeletonComponent>();
    if (!pSkel) return;

    const auto& ents = pool->Entities();
    auto& data = pool->Data();
    for (size_t i = 0; i < data.size(); ++i)
    {
        const Entity e = ents[i];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        FaceTrackingComponent& ft = data[i];
        if (!ft.enabled || !ft.rt) continue;
        FT::BindingRuntime& rt = *ft.rt;
        if (rt.owner != e || (!rt.haveHead && !rt.haveEyes && !rt.haveJaw)) continue;
        const SkeletonComponent* skc = pSkel->Get(e);
        if (!skc || skc->assetIndex != rt.bonesFor || skc->assetIndex >= m_skeletons.Count()) continue;
        const SkeletonAsset& skel = m_skeletons.Get(skc->assetIndex);
        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        // The journal knows the pose cache's real size (SkeletonComponent::boneCount): a bone it
        // cannot record is a bone this must not write - it would never be put back.
        PoseBinding(skel, rt, poses,
                    [&](int bone) { return m_animSys.JournalBoneRotation(world, e, static_cast<uint32_t>(bone)); });
    }
}

// ==== Bake ====

bool FaceTrackingSystem::Bake(World& world, SkeletonRegistry& skeletons, Entity character, const FT::FaceTake& take,
                              const BakeOptions& opts,
                              Resource::AnimationResource& out, BakeReport& report, std::string& error)
{
    report = BakeReport{};
    if (!(opts.fps >= 1.0f && opts.fps <= 240.0f))
    {
        error = "fps is 1..240";
        return false;
    }
    if (take.frames.empty())
    {
        error = "the take has no frames";
        return false;
    }
    const Entity carrier = character == NullEntity ? NullEntity : Facial::ResolveSkeletonCarrier(world, character);
    const SkeletonComponent* skc = carrier == NullEntity ? nullptr : world.GetComponent<SkeletonComponent>(carrier);
    if (!skc || skc->assetIndex == kInvalidAnimHandle || skc->assetIndex >= skeletons.Count())
    {
        error = "the character has no loaded skeleton";
        return false;
    }
    SkeletonAsset& skel = skeletons.Get(skc->assetIndex);

    // The character's own binding settings when it has one (mapping, weights, timings),
    // with the bake's mirror (a bake is unmirrored unless asked).
    FaceTrackingComponent ft;
    if (const FaceTrackingComponent* bound = world.GetComponent<FaceTrackingComponent>(carrier))
        ft = ComponentCloner<FaceTrackingComponent>::Clone(*bound);
    ft.enabled = true;
    ft.mirror = opts.mirror;
    FT::BindingRuntime rt;
    rt.owner = carrier;
    rt.ownerGeneration = world.GetGeneration(carrier);
    rt.sourceName = ft.source;
    rt.mirror = ft.mirror;
    Compile(world, carrier, ft, skel, skc->assetIndex, rt, ProfileGeneration(), &take.channels);
    if (!rt.compiled)
    {
        error = "the mapping was refused: " + rt.compileError;
        return false;
    }
    report.profile = rt.profilePath;
    report.warnings = rt.warnings;
    // The performer's calibration, when the bake names the source it was captured on.
    if (!opts.calibration.empty())
    {
        uint64_t generation = 0;
        if (!FT::Service::Get().GetCalibration(opts.calibration, rt.calibration, generation))
            report.warnings.push_back("no calibration is kept for source '" + opts.calibration + "': the take is baked uncalibrated");
    }

    // The keys: from the take's time 0 - where a replay of it starts - to its last frame. Frame
    // times are whole microseconds (1/60 s is 16667 us), so the last frame of a take recorded at
    // the key rate is allowed to sit one microsecond short of its key.
    FT::FaceTakePlayer player;
    player.Reset(&take.frames, /*loop=*/false);
    const double stepUs = 1e6 / static_cast<double>(opts.fps);
    const double keysD = std::floor((static_cast<double>(player.SpanUs()) + 1.0) / stepUs) + 1.0;
    if (keysD > static_cast<double>(kMaxBakeKeys))
    {
        error = "the take is longer than " + std::to_string(kMaxBakeKeys) + " keys at this fps";
        return false;
    }
    const uint32_t keys = static_cast<uint32_t>(keysD);

    // The bones the clip keys: the ones the mapping turns, from the Neck down - the
    // AnimLayer that plays it masks from there (BoneMaskMode::CustomRoot). A chain bone
    // above the Neck (a chest share) is folded into the Neck's key, so the Neck and all
    // below it land where a live binding put them; the chest itself stays as the layer
    // below has it.
    std::vector<int> written;
    for (int k = 0; k < rt.chainCount; ++k) written.push_back(rt.chainBone[k]);
    for (int b : {rt.leftEye, rt.rightEye, rt.jawBone})
        if (b >= 0) written.push_back(b);
    int root = -1;
    if (!written.empty())
    {
        root = ResolveRole(skel, HumanoidBone::Neck);
        if (root < 0 || rt.headBone < 0 || !IsAncestorOrSelf(skel, root, rt.headBone)) root = rt.headBone;
        if (root < 0) root = *std::min_element(written.begin(), written.end());
    }
    std::vector<int> bones;
    if (root >= 0) bones.push_back(root);
    for (int b : written)
    {
        if (b == root || std::find(bones.begin(), bones.end(), b) != bones.end()) continue;
        if (IsAncestorOrSelf(skel, root, b)) bones.push_back(b);
        else if (IsAncestorOrSelf(skel, b, root)) ++report.folded;
        else report.warnings.push_back(std::string(skel.boneNames[b]) + " is not under " + skel.boneNames[root] + ": not baked");
    }
    std::sort(bones.begin(), bones.end()); // parents first
    report.root = root >= 0 ? skel.boneNames[root] : "";

    std::vector<AnimationSystem::LocalPose> rest(skel.boneCount), pose(skel.boneCount);
    Anim::PoseOps::RefPose(skel, rest.data());
    const int rootParent = root >= 0 ? skel.parentIndex[root] : -1;
    const XMVECTOR rootParentRest = rootParent >= 0 ? RestModelRotation(skel, rootParent) : XMQuaternionIdentity();
    std::vector<std::vector<XMFLOAT4>> rot(bones.size(), std::vector<XMFLOAT4>(keys));
    std::vector<std::vector<float>> morph(skel.morphTargetCount);
    const auto noJournal = [](int) { return true; };

    FT::FaceAuthorityParams ap{std::max(0.0f, ft.holdSec), std::max(0.0f, ft.fadeOutSec), std::max(0.0f, ft.fadeInSec)};
    bool reachedFull = false; // until the face is first at full authority it lands at once
    for (uint32_t k = 0; k < keys; ++k)
    {
        const int64_t tk = static_cast<int64_t>(std::llround(static_cast<double>(k) * stepUs));
        int64_t foundUs = -1;
        player.Advance(tk, [&](const FT::FaceFrame& emitted) {
            FT::FaceFrame f = emitted;
            ConsumeFrame(rt, ft.mirror, f, foundUs);
            return true;
        });
        rt.processed.Trim();
        rt.nowUs = tk;
        FT::FaceAuthorityParams step = ap;
        if (!reachedFull) step.fadeInSec = step.fadeOutSec = 0.0f;
        rt.authority.Update(tk, foundUs, rt.newestFound, step);
        rt.sampleState = rt.processed.Sample(tk, rt.sampled);
        const float a = rt.authority.Value();
        reachedFull = reachedFull || a >= 1.0f;
        const bool wasBlinkHeld = rt.blinkHeld;
        ClearOutputs(rt);
        rt.authorityValue = a;
        if (rt.sampleState != FT::SampleState::Empty && a > 0.0f)
            EvaluateBinding(ft, rt, skel, nullptr, a, wasBlinkHeld, /*wantWeights=*/true);

        if (rt.morphAlpha > 0.0f)
            for (const auto& [slot, w] : rt.slotWeights)
            {
                if (slot < 0 || static_cast<uint32_t>(slot) >= skel.morphTargetCount) continue;
                std::vector<float>& track = morph[static_cast<size_t>(slot)];
                if (track.empty()) track.assign(keys, 0.0f);
                track[k] = Sat(w) * rt.morphAlpha;
            }

        pose = rest;
        PoseBinding(skel, rt, pose.data(), noJournal);
        for (size_t i = 0; i < bones.size(); ++i)
        {
            const int b = bones[i];
            XMVECTOR q = XMLoadFloat4(&pose[static_cast<size_t>(b)].rot);
            // The root under its parent's REST rotation, so it reaches the turn the folded bones
            // above it gave it: Model(root) = QMul(local, Model(parent)).
            if (b == root) q = QMul(ModelRotation(pose.data(), skel, root), FP::Inv(rootParentRest));
            XMStoreFloat4(&rot[i][k], XMQuaternionNormalize(q));
        }
    }

    // ---- the clips: one bone clip, one morph clip, of one name ----
    const std::string name = opts.clipName.empty() ? std::string("FaceTake") : opts.clipName;
    const float duration = keys > 1 ? static_cast<float>(keys - 1) / opts.fps : 0.0f;
    out.clips.clear();
    out.morphClips.clear();
    out.cameraClips.clear();
    if (!bones.empty())
    {
        Resource::AnimClipData clip;
        strncpy_s(clip.name, name.c_str(), _TRUNCATE);
        clip.frameCount = keys;
        clip.frameRate = opts.fps;
        clip.duration = duration;
        for (size_t i = 0; i < bones.size(); ++i)
        {
            const AnimationSystem::LocalPose& r = rest[static_cast<size_t>(bones[i])];
            Resource::AnimClipChannel ch;
            strncpy_s(ch.name, skel.boneNames[bones[i]], _TRUNCATE);
            // Every track as long as the clip: a short one is padded with zeros on load.
            ch.positions.assign(keys, r.pos);
            ch.scales.assign(keys, r.scl);
            ch.rotations = std::move(rot[i]);
            clip.channels.push_back(std::move(ch));
        }
        out.clips.push_back(std::move(clip));
    }
    Resource::MorphClipData mc;
    strncpy_s(mc.name, name.c_str(), _TRUNCATE);
    mc.frameCount = keys;
    mc.frameRate = opts.fps;
    mc.duration = duration;
    for (uint32_t slot = 0; slot < skel.morphTargetCount; ++slot)
    {
        if (morph[slot].empty()) continue;
        Resource::MorphClipChannel ch;
        strncpy_s(ch.name, skel.morphTargetNames[slot], _TRUNCATE);
        ch.weights = std::move(morph[slot]);
        mc.channels.push_back(std::move(ch));
    }
    if (!mc.channels.empty()) out.morphClips.push_back(std::move(mc));
    if (out.clips.empty() && out.morphClips.empty())
    {
        error = "the mapping drives nothing on this character from this take";
        return false;
    }
    report.keys = keys;
    report.duration = duration;
    report.framesRead = rt.framesConsumed;
    report.bones = out.clips.empty() ? 0 : out.clips.front().channels.size();
    report.morphs = out.morphClips.empty() ? 0 : out.morphClips.front().channels.size();
    LOG_INFO("[FACETRACK] bake entity=%u profile=%s keys=%u fps=%.3f frames=%llu morphs=%zu bones=%zu root=%s folded=%d "
             "mirror=%d warnings=%zu",
             static_cast<unsigned>(carrier), rt.profilePath.c_str(), keys, static_cast<double>(opts.fps),
             static_cast<unsigned long long>(rt.framesConsumed), report.morphs, report.bones,
             report.root.empty() ? "-" : report.root.c_str(), report.folded, opts.mirror ? 1 : 0, report.warnings.size());
    return true;
}

bool FaceTrackingSystem::BakeTakeFile(World& world, SkeletonRegistry& skeletons, Entity character, const std::string& takePath,
                                      const std::string& outPath, const BakeOptions& options, BakeReport& report,
                                      std::string& error)
{
    std::string text;
    if (!Resource::AssetFS::Get().ReadFileText(takePath, text))
    {
        error = "cannot read '" + takePath + "'";
        return false;
    }
    FT::FaceTake take;
    std::istringstream in(text);
    if (!FT::ParseTake(in, take, error))
    {
        error = takePath + ": " + error;
        return false;
    }
    BakeOptions opts = options;
    if (opts.clipName.empty()) opts.clipName = Sys::PathToUtf8(Sys::U8Path(takePath).stem());
    Resource::AnimationResource res;
    if (!Bake(world, skeletons, character, take, opts, res, report, error)) return false;
    const std::vector<uint8_t> blob = Resource::SerializeAnimation(res);
    if (blob.empty())
    {
        error = "the clips serialized to nothing";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path parent = Sys::U8Path(outPath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (!Sys::WriteFileBytesAtomic(outPath, blob.data(), blob.size()))
    {
        error = "cannot write '" + outPath + "'";
        return false;
    }
    LOG_INFO("[FACETRACK] baked '%s' -> '%s' (%zu bytes)", takePath.c_str(), outPath.c_str(), blob.size());
    return true;
}
