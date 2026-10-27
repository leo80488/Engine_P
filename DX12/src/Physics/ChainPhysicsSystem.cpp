#include "Physics/ChainPhysicsSystem.h"
#include "Physics/ChainBoneNames.h"
#include "Physics/ChainPhysicsMath.h"
#include "Graphics/AnimProfile.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "System/TaskSystem.h"
#include "System/TaskGraph.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include <limits>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace DirectX;

// ==== Helpers ====
struct SkirtID { int group; int chain; };

static XMMATRIX LocalPoseToMat(const AnimationSystem::LocalPose& p)
{
    XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
    XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
    XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
    return S * R * T;
}

static float Distance3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrtf(dx * dx + dy * dy + dz * dz);
}

// Check if a bone name matches any physics pattern.
// Returns: 0 = not physics, 1 = hair, 2 = skirt, 3 = spring bone (jiggle).
// The rule lives in Physics/ChainBoneNames.h so the Skeleton Editor can show it.
static int ClassifyBone(const char* name)
{
    return ChainPhysicsNames::ClassifyBone(name);
}

// Parses a skirt bone name "Skirt_X_Y" -> group X, chain Y.
// X = constraint group (all chains of one X share horizontal / shear constraints),
// Y = chain index (position around the ring); "Skirt_X" alone is group 0, chain X.
// Direction names ("Skirt_LF", "Skirt_MF", ...) are matched first, as substrings.
// A name in neither form gives chain -1, which InitEntity auto-orders after the
// explicit indices of its ring group.
static SkirtID ParseSkirtID(const char* name)
{
    std::string s(name);

    // Direction names (Skirt_LF, Skirt_MF, ...) -> group 0, chain from this table.
    struct DirMap { const char* tag; int idx; };
    static const DirMap kDirs[] = {
        { "LF", 0 }, { "MF", 1 }, { "RF", 2 },
        { "RB", 3 }, { "MB", 4 }, { "LB", 5 },
        { "FL", 0 }, { "FM", 1 }, { "FR", 2 },
        { "BR", 3 }, { "BM", 4 }, { "BL", 5 },
    };
    for (const auto& d : kDirs)
    {
        if (s.find(d.tag) != std::string::npos)
            return { 0, d.idx };
    }

    // Numeric naming: "Skirt_X_Y" where X = group, Y = chain
    auto pos = s.find("Skirt_");
    if (pos == std::string::npos) pos = s.find("skirt_");
    if (pos != std::string::npos)
    {
        pos += 6; // skip "Skirt_" / "skirt_"
        int x = 0;
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                x = x * 10 + (s[pos++] - '0');
        }
        else return { 0, -1 };

        if (pos < s.size() && s[pos] == '_') ++pos;

        int y = 0;
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                y = y * 10 + (s[pos++] - '0');
            return { x, y };
        }

        // Only X present (e.g. "Skirt_3"), treat as single group, X = chain
        return { 0, x };
    }

    return { 0, -1 };
}

// Build children list for skeleton (SkeletonAsset only stores parentIndex).
static void BuildChildrenList(const SkeletonAsset& skel,
                              std::vector<std::vector<uint32_t>>& children)
{
    children.resize(skel.boneCount);
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        int32_t p = skel.parentIndex[b];
        if (p >= 0 && static_cast<uint32_t>(p) < skel.boneCount)
            children[p].push_back(b);
    }
}

// Resolves a bone NAME to its skeleton index, or ~0u when not found / empty. A
// linear scan (boneCount <= 1024, init / authoring only, never per frame) avoids
// depending on the importer's private FNV-32 keying of SkeletonAsset.nameToIndex.
static uint32_t ResolveBoneByName(const SkeletonAsset& skel, const char* name)
{
    if (!name || !name[0]) return ~0u;
    // Real bones only: a virtual bone is never simulated.
    for (uint32_t b = 0; b < skel.RealBoneCount(); ++b)
        if (std::strcmp(skel.boneNames[b], name) == 0) return b;
    return ~0u;
}

// ==== World-motion inertia ====
namespace
{
    float WorldShare01(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }   // NaN -> 0

    // FeltKick is ONE strand's (or spring's) share of a pass's entity motion
    // (ChainPhysicsComponent::worldInertiaLocation / Rotation; the header describes
    // the model, PrepareWorldKick builds the unscaled deltas).
    //
    // The frame delta D maps the previous pass's skeleton space onto this one for
    // a WORLD-fixed point: p' = p R + t. A SKELETON-fixed point at x therefore
    // moves relative to the world at V(x) = (x - (x R + t)) / dt, and a particle
    // whose WORLD velocity is conserved across the frame change leaves it with
    //   v' = v R + (V_prev(x) R - V(x))       (velocities in skeleton space)
    // - re-expressed in the new orientation, minus the change of the frame's own
    // velocity at x. At constant entity velocity the bracket is 0 (a steady walk
    // is invisible); a start / stop / turn is a kick; a steady spin leaves the
    // centrifugal term (V_prev(x) R - V(x) = -dt w x (w x x)). The share scales D
    // (rotation slerped from identity, translation scaled), both deltas alike.
    struct FeltKick
    {
        XMVECTOR q, qPrev, t, tPrev;
        float    invDt = 0.f, invDtPrev = 0.f;
        bool     rot = false;

        // Returns false (no kick) when both shares are zero or a dt is not positive.
        bool Resolve(const XMFLOAT4& rot4, const XMFLOAT3& pos3, float dt,
                     const XMFLOAT4& prevRot4, const XMFLOAT3& prevPos3, float prevDt,
                     float shareLoc, float shareRot)
        {
            shareLoc = WorldShare01(shareLoc);
            shareRot = WorldShare01(shareRot);
            if (shareLoc <= 0.f && shareRot <= 0.f) return false;
            if (!(dt > 0.f) || !(prevDt > 0.f)) return false;
            // An entity that did not turn in either pass takes the exact
            // translation-only path (PrepareWorldKick writes an EXACT identity
            // for an unchanged orientation), so a steady / static entity solves
            // bit-identically to one without world inertia, not within rounding.
            auto isId = [](const XMFLOAT4& r)
            { return r.x == 0.f && r.y == 0.f && r.z == 0.f && r.w == 1.f; };
            rot = shareRot > 0.f && !(isId(rot4) && isId(prevRot4));
            const XMVECTOR id = XMQuaternionIdentity();
            q     = rot ? XMQuaternionSlerp(id, XMLoadFloat4(&rot4), shareRot) : id;
            qPrev = rot ? XMQuaternionSlerp(id, XMLoadFloat4(&prevRot4), shareRot) : id;
            t     = XMVectorScale(XMLoadFloat3(&pos3), shareLoc);
            tPrev = XMVectorScale(XMLoadFloat3(&prevPos3), shareLoc);
            invDt = 1.f / dt;
            invDtPrev = 1.f / prevDt;
            return true;
        }
        // V_prev(x) R - V(x). Translation only (no rotation share): the frame
        // velocity is the same at every x, V = -t / dt.
        XMVECTOR Delta(FXMVECTOR x) const
        {
            if (!rot)
                return XMVectorSubtract(XMVectorScale(t, invDt), XMVectorScale(tPrev, invDtPrev));
            const XMVECTOR vPrev = XMVectorScale(
                XMVectorSubtract(XMVectorSubtract(x, XMVector3Rotate(x, qPrev)), tPrev), invDtPrev);
            const XMVECTOR vNow = XMVectorScale(
                XMVectorSubtract(XMVectorSubtract(x, XMVector3Rotate(x, q)), t), invDt);
            return XMVectorSubtract(XMVector3Rotate(vPrev, q), vNow);
        }
        // A velocity re-expressed in the new orientation (v R).
        XMVECTOR Turn(FXMVECTOR v) const { return rot ? XMVector3Rotate(v, q) : v; }
    };
}

// ==== Authored rest pose: ApplyRestPose / IsRestPosedBone ====
// ApplyRestPose stamps ChainPhysicsComponent::restPose into the LocalPose array.
// It runs in RunPass's SERIAL phase 1 (and in HoldPass, and in ApplyRestPoseOnly
// for Edit / Pause) before InitEntity, before resetRot and before the FK anchor
// cache; BoneRestPose (in the header) explains why that order turns an authored
// pose into the solver's rest shape instead of a one-shot pose.
void ChainPhysicsSystem::ApplyRestPose(ChainPhysicsComponent& cfg,
                                       const SkeletonAsset&   skel,
                                       uint32_t               skelAssetIndex,
                                       AnimationSystem::LocalPose* poses)
{
    if (!poses) return;

    // Cleared UNCONDITIONALLY, before any early-out. A stale `true` left over
    // from last frame would keep resetRot skipping a bone that nothing re-bases,
    // which is exactly the self-chasing drift resetRot exists to prevent.
    for (BoneRestPose& r : cfg.restPose) r.appliedThisFrame = false;

    if (!cfg.restPoseEnabled || cfg.restPose.empty() || cfg.restPoseWeight <= 0.f)
        return;

    for (BoneRestPose& r : cfg.restPose)
    {
        if (!r.enabled || r.boneName[0] == '\0') continue;

        const bool cacheStale = (r.resolvedSkel != skelAssetIndex)
                             || (std::strcmp(r.resolvedName, r.boneName) != 0);
        if (cacheStale)
        {
            r.cachedBoneIdx = ResolveBoneByName(skel, r.boneName);
            snprintf(r.resolvedName, sizeof(r.resolvedName), "%s", r.boneName);
            r.resolvedSkel  = skelAssetIndex;
            r.resolveFailed = (r.cachedBoneIdx >= skel.boneCount);
            r.warnedMissing = false;
        }
        else if (!r.resolveFailed &&
                 (r.cachedBoneIdx >= skel.boneCount ||
                  std::strcmp(skel.boneNames[r.cachedBoneIdx], r.boneName) != 0))
        {
            // The cache is keyed on the skeleton ASSET INDEX, and an index can be
            // re-registered with different content (asset hot-swap, world reload
            // reusing slot N). Then resolvedSkel still matches and a stale index
            // would silently pose a DIFFERENT bone. One strcmp per entry per frame
            // makes the cache self-validating; the O(entries x boneCount) rescan
            // only runs when it actually went stale.
            r.cachedBoneIdx = ResolveBoneByName(skel, r.boneName);
            r.resolveFailed = (r.cachedBoneIdx >= skel.boneCount);
        }

        if (r.resolveFailed)
        {
            // Latched: without this the guard above would be true again next
            // frame and re-scan every name against every bone, forever, behind a
            // single log line.
            if (!r.warnedMissing)
            {
                LOG_WARNING("ChainPhysics: rest-pose bone '%s' not found in skeleton - skipped",
                            r.boneName);
                r.warnedMissing = true;
            }
            continue;
        }
        const uint32_t b = r.cachedBoneIdx;

        // Sanitise the weights like the quaternion below: a non-finite weight makes
        // XMQuaternionSlerp emit NaN, which reaches the skin matrix and
        // access-violates the BVH build.
        float w = cfg.restPoseWeight * r.weight;
        if (!std::isfinite(w)) continue;
        w = (w < 0.f) ? 0.f : (w > 1.f ? 1.f : w);
        if (w <= 0.0001f) continue;   // nothing written => appliedThisFrame stays false

        XMVECTOR authored = XMLoadFloat4(&r.rot);
        // A non-finite or zero-length quaternion (hand-edited scene, truncated
        // line) would normalize to NaN, and that NaN reaches the skin matrix and
        // access-violates SceneBVH::BuildRecursive. The reader guards too; this
        // guards the runtime-authoring path.
        if (!XMVector4IsNaN(authored) && !XMVector4IsInfinite(authored) &&
            XMVectorGetX(XMVector4LengthSq(authored)) > 1e-12f)
            authored = XMQuaternionNormalize(authored);
        else
            authored = XMQuaternionIdentity();

        if (w >= 0.999f)                              // idempotent fast path
        {
            XMStoreFloat4(&poses[b].rot, authored);
            r.appliedThisFrame = true;
            continue;
        }

        // The blend base is ALWAYS skel.restPoseLocal[b], NEVER poses[b].rot, which
        // holds our own previous output when (a) there is no live pose source
        // (AnimationSystem seeds the pose once), (b) a clip plays but
        // clip.boneCount < skel.boneCount, so SampleClip rewrites only the first n
        // bones while "a clip is active", or (c) WriteBonePoses replaced .rot for
        // i >= 1. That base makes a partial weight creep toward 1 over a few seconds;
        // restPoseLocal is a constant asset value.
        //
        // Consequence, stated in the UI tooltip: restPoseWeight fades from the
        // MODELLED shape to the authored shape; it is not an animation-vs-authored
        // blend.
        XMVECTOR s, q, t;
        const XMVECTOR base =
            XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&skel.restPoseLocal[b]))
            ? XMQuaternionNormalize(q) : XMQuaternionIdentity();

        XMStoreFloat4(&poses[b].rot,
            XMQuaternionNormalize(XMQuaternionSlerp(base, authored, w)));
        r.appliedThisFrame = true;
    }
}

bool ChainPhysicsSystem::IsRestPosedBone(const ChainPhysicsComponent& cfg, uint32_t bone)
{
    // Reads ONLY appliedThisFrame. Re-deriving the condition here (enabled /
    // effective weight / name validity / resolution) would eventually disagree
    // with ApplyRestPose, and the moment we skip more bones than we wrote,
    // resetRot leaves a bone with no clean base and the chain chases itself.
    for (const BoneRestPose& r : cfg.restPose)
        if (r.appliedThisFrame && r.cachedBoneIdx == bone) return true;
    return false;
}

// ==== ComputeWorldTransform: FK from the current poses, as IKSystem ====
XMMATRIX ChainPhysicsSystem::ComputeWorldTransform(
    uint32_t boneIndex,
    const AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel) const
{
    uint32_t chain[64]; // 256 bytes on the stack (not a boneCount array): stays in L1
    uint32_t depth = 0;
    int32_t  cur   = static_cast<int32_t>(boneIndex);
    while (cur >= 0 && depth < 64)
    {
        chain[depth++] = static_cast<uint32_t>(cur);
        cur = skel.parentIndex[cur];
    }

    XMMATRIX world = XMMatrixIdentity();
    for (uint32_t i = depth; i > 0; --i)
        world = LocalPoseToMat(poses[chain[i - 1]]) * world;

    return world;
}

// ==== ComputeBindWorldPos: a bone's world position from the current poses ====
XMFLOAT3 ChainPhysicsSystem::ComputeBindWorldPos(
    uint32_t bone,
    const SkeletonAsset& skel,
    const AnimationSystem::LocalPose* poses)
{
    XMMATRIX w = ComputeWorldTransform(bone, poses, skel);
    XMFLOAT3 pos;
    XMStoreFloat3(&pos, w.r[3]);
    return pos;
}

// ==== TraceChains: DFS that forks at branches, one chain per leaf path ====
void ChainPhysicsSystem::TraceChains(
    uint32_t root,
    const SkeletonAsset& skel,
    const std::vector<bool>& isPhysBone,
    const std::vector<std::vector<uint32_t>>& children,
    std::vector<uint32_t>& current,
    std::vector<std::vector<uint32_t>>& outChains)
{
    current.push_back(root);

    std::vector<uint32_t> physChildren;
    for (uint32_t child : children[root])
        if (child < skel.boneCount && isPhysBone[child])
            physChildren.push_back(child);

    if (physChildren.empty())
    {
        // Tip reached: the current path is one complete chain.
        outChains.push_back(current);
    }
    else
    {
        // Recurse into each physics child (fork = separate chains)
        for (uint32_t child : physChildren)
            TraceChains(child, skel, isPhysBone, children, current, outChains);
    }

    current.pop_back();
}

// ==== InitEntity: scan the skeleton, detect chains, build particles and constraints ====
void ChainPhysicsSystem::InitEntity(
    Entity e,
    const ChainPhysicsComponent& cfg,
    const SkeletonAsset& skel,
    const AnimationSystem::LocalPose* poses)
{
    PROF_SCOPE("Chain.InitEntity");
    EntityData& data = m_entityData[e];
    data = EntityData{};

    // Step 1: classify every bone -> boneClass[] (0 = none, 1 = hair, 2 = skirt,
    // 3 = spring) and isPhysBone[]. The source depends on cfg.groups:
    //   empty     : bone-NAME keyword classification (scenes without groups)
    //   non-empty : explicit authored groups (KawaiiPhysics-style root + exclude)
    std::vector<int>  boneClass (skel.boneCount, 0);
    std::vector<bool> isPhysBone(skel.boneCount, false);

    // Per-bone authoring metadata consumed by the skirt (Step 5) and spring
    // (Step 7) builders, so those stay agnostic to how a bone got classified:
    //   ringGroupOf/ringIndexOf : for a skirt ROOT, its ParseSkirtID-equivalent ids
    //   springPairId            : spring bones sharing a value >=0 form a virtual root
    std::vector<int> ringGroupOf (skel.boneCount, 0);
    std::vector<int> ringIndexOf (skel.boneCount, -1);
    std::vector<int> springPairId(skel.boneCount, -1);

    // bone -> source ChainGroupDef index (authored path only; -1 = none / keyword
    // path). Threaded onto each Strand / Constraint / SpringBone so Simulate can
    // resolve the group's per-chain overrides (else the type-globals apply).
    std::vector<int> boneGroup(skel.boneCount, -1);

    const bool authored = !cfg.groups.empty();

    // Authored groups stay separate all the way through chain tracing: one shared
    // isPhysBone[] mask would trace nested, adjacent or overlapping groups as one
    // physics island.
    struct AuthoredGroupRuntime
    {
        uint32_t          root = ~0u;
        std::vector<bool> mask; // exclusive, post-exclusion bone ownership
    };
    std::vector<AuthoredGroupRuntime> authoredGroups;
    if (authored)
    {
        authoredGroups.resize(cfg.groups.size());
        for (auto& runtime : authoredGroups)
            runtime.mask.assign(skel.boneCount, false);
    }

    // Step 2: Build children list once (shared by TraceChains + authored mark).
    std::vector<std::vector<uint32_t>> children;
    BuildChildrenList(skel, children);

    if (!authored)
    {
        // ---- Keyword path (no authored groups) ----
        // Real bones only: a virtual bone named "VB Hair_..." is not a hair strand.
        for (uint32_t b = 0; b < skel.RealBoneCount(); ++b)
        {
            boneClass[b]  = ClassifyBone(skel.boneNames[b]);
            isPhysBone[b] = (boneClass[b] != 0);

            if (boneClass[b] == 2)
            {
                SkirtID sid = ParseSkirtID(skel.boneNames[b]);
                ringGroupOf[b] = sid.group;
                ringIndexOf[b] = sid.chain;
            }
            else if (boneClass[b] == 3)
            {
                // Chest bones sharing a parent pair up under a virtual root;
                // Butt (and lone chest) stay independent. Encode "shared parent"
                // as the pair id so Step 7 groups them identically.
                std::string nm(skel.boneNames[b]);
                if (nm.find("Chest") != std::string::npos ||
                    nm.find("chest") != std::string::npos)
                    springPairId[b] = skel.parentIndex[b];
            }
        }
    }
    else
    {
        // ---- Authored group path (KawaiiPhysics-style) ----
        // For each enabled group: resolve the root bone by name, mark its whole
        // descendant subtree (one forward pass, valid because parentIndex[i] < i),
        // subtract excluded bones / subtrees; the ownership pass below then stamps
        // boneClass[].
        for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
        {
            const ChainGroupDef& g = cfg.groups[gi];
            if (!g.enabled) continue;

            const uint32_t root = ResolveBoneByName(skel, g.rootBone);
            if (root >= skel.boneCount)
            {
                LOG_WARNING("ChainPhysics: group '%s' root bone '%s' not found in skeleton — skipped",
                            g.name[0] ? g.name : "(unnamed)",
                            g.rootBone[0] ? g.rootBone : "(empty)");
                continue;
            }

            AuthoredGroupRuntime& runtime = authoredGroups[gi];
            runtime.root = root;

            if (g.type == ChainGroupType::Spring)
            {
                // A Spring group owns only its root bone.
                runtime.mask[root] = true;
                continue;
            }

            // Mark root + descendant subtree.
            std::vector<bool> marked(skel.boneCount, false);
            marked[root] = true;
            // A virtual bone under the root is not the group's: real bones only.
            for (uint32_t b = root + 1; b < skel.RealBoneCount(); ++b)
                if (skel.parentIndex[b] >= 0 &&
                    marked[static_cast<uint32_t>(skel.parentIndex[b])])
                    marked[b] = true;

            // Subtract excludes: seed excluded bones, then (optionally) cascade
            // the exclusion down their subtrees with the same forward pass.
            std::vector<bool> excluded(skel.boneCount, false);
            for (int j = 0; j < g.excludeCount && j < ChainGroupDef::MAX_EXCLUDE; ++j)
            {
                uint32_t ex = ResolveBoneByName(skel, g.excludeBones[j]);
                if (ex < skel.boneCount) excluded[ex] = true;
            }
            if (g.excludeSubtree)
                for (uint32_t b = 0; b < skel.boneCount; ++b)
                    if (skel.parentIndex[b] >= 0 &&
                        excluded[static_cast<uint32_t>(skel.parentIndex[b])])
                        excluded[b] = true;

            for (uint32_t b = 0; b < skel.boneCount; ++b)
                if (marked[b] && !excluded[b])
                    runtime.mask[b] = true;
        }

        // One LocalPose bone cannot be written by two physics islands. Claim
        // deeper group roots first, so a nested group cleanly carves its subtree
        // out of a broader parent group. Equal-depth ties keep component order;
        // a Spring on the exact same root wins because it owns only that bone.
        auto boneDepth = [&](uint32_t bone)
        {
            int depth = 0;
            int32_t p = (bone < skel.boneCount) ? skel.parentIndex[bone] : -1;
            while (p >= 0 && static_cast<uint32_t>(p) < skel.boneCount &&
                   depth < static_cast<int>(skel.boneCount))
            {
                ++depth;
                p = skel.parentIndex[static_cast<uint32_t>(p)];
            }
            return depth;
        };

        std::vector<int> ownershipOrder;
        ownershipOrder.reserve(cfg.groups.size());
        for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
            if (authoredGroups[gi].root < skel.boneCount)
                ownershipOrder.push_back(gi);

        std::sort(ownershipOrder.begin(), ownershipOrder.end(),
            [&](int a, int b)
            {
                const int da = boneDepth(authoredGroups[a].root);
                const int db = boneDepth(authoredGroups[b].root);
                if (da != db) return da > db;
                const bool aSpring = cfg.groups[a].type == ChainGroupType::Spring;
                const bool bSpring = cfg.groups[b].type == ChainGroupType::Spring;
                if (aSpring != bSpring) return aSpring;
                return a < b;
            });

        std::vector<int> boneOwner(skel.boneCount, -1);
        for (int gi : ownershipOrder)
        {
            AuthoredGroupRuntime& runtime = authoredGroups[gi];
            int overlapCount = 0;
            for (uint32_t b = 0; b < skel.boneCount; ++b)
            {
                if (!runtime.mask[b]) continue;
                if (boneOwner[b] >= 0)
                {
                    runtime.mask[b] = false;
                    ++overlapCount;
                }
                else
                {
                    boneOwner[b] = gi;
                }
            }

            if (overlapCount > 0)
            {
                const ChainGroupDef& g = cfg.groups[gi];
                const char* gname = g.name[0] ? g.name : "(unnamed)";
                // Deduplicated by (group name, count), not per entity: this is a fact about
                // the AUTHORED config, so every prefab instance would log the identical
                // line, burying the log and loading the async logger. The first occurrence
                // is the useful one.
                static std::set<std::pair<std::string, int>> s_reported;
                static std::mutex s_reportMtx;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lk(s_reportMtx);
                    first = s_reported.emplace(gname, overlapCount).second;
                }
                if (first)
                {
                    LOG_WARNING("ChainPhysics: group '%s' dropped %d overlapping bones so authored "
                                "physics groups stay isolated (authored-config issue — logged once "
                                "per distinct group; every entity using this config hits it)",
                                gname, overlapCount);
                }
            }
        }

        // Publish exclusive ownership for the common strand/spring builders.
        for (uint32_t b = 0; b < skel.boneCount; ++b)
        {
            const int gi = boneOwner[b];
            if (gi < 0) continue;

            const ChainGroupDef& g = cfg.groups[gi];
            boneClass[b]  = static_cast<int>(g.type) + 1;
            isPhysBone[b] = true;
            boneGroup[b]  = gi;

            if (g.type == ChainGroupType::Spring)
                springPairId[b] = (g.pairGroupId >= 0) ? g.pairGroupId : -1;
        }
    }

    // Step 2b: drop dummy leaves hanging off a fork.
    //
    // PMX physics tools append a leaf "weight" bone (the rigid-body anchor) to
    // EVERY joint of a chain, so a converted rig reads
    //     hair_01 -> { hair_02, hair_01-weight }
    //     hair_02 -> { hair_03, hair_02-weight } ...
    // Every joint is a fork, and TraceChains forks into one strand per leaf path,
    // so such a rig traces one extra strand per weight bone.
    //
    // A leaf child at a fork that has a non-leaf sibling is never the longest path
    // through the fork bone, so under the one-writer-per-bone rule
    // (Particle::writesBone) its strand writes nothing: pure solver cost plus a
    // bogus ring member for an authored Cloth root. Drop it. A leaf that is the
    // ONLY physics child (the MMD tip bone, or the weight bone of a chain's last
    // joint) is what lets its parent rotate, and stays.
    //
    // Consequence for rootSolo below: a root whose only sibling branch was a
    // weight bone becomes solo and is aim-rotated toward its child, as the
    // KawaiiPhysics parity rule intends.
    {
        auto hasPhysChild = [&](uint32_t b, const std::vector<bool>& mask)
        {
            for (uint32_t c : children[b])
                if (c < skel.boneCount && mask[c]) return true;
            return false;
        };
        auto pruneDummyLeaves = [&](std::vector<bool>& mask, int srcGroup)
        {
            for (uint32_t b = 0; b < skel.boneCount; ++b)
            {
                if (!mask[b]) continue;
                bool anyNonLeaf = false;
                for (uint32_t c : children[b])
                    if (c < skel.boneCount && mask[c] && hasPhysChild(c, mask))
                    { anyNonLeaf = true; break; }
                if (!anyNonLeaf) continue;
                for (uint32_t c : children[b])
                {
                    if (c >= skel.boneCount || !mask[c] || hasPhysChild(c, mask)) continue;
                    if (boneClass[c] == 3) continue; // a jiggle bone is a spring, not a strand tip
                    mask[c] = false;
                    // The shared per-bone tables must agree with the trace mask,
                    // or Step 3 would still see the leaf as a physics bone.
                    if (!authored || boneGroup[c] == srcGroup)
                    {
                        isPhysBone[c] = false;
                        boneClass[c]  = 0;
                        boneGroup[c]  = -1;
                    }
                }
            }
        };
        if (!authored)
        {
            pruneDummyLeaves(isPhysBone, -1);
        }
        else
        {
            for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
            {
                if (!cfg.groups[gi].enabled ||
                    cfg.groups[gi].type == ChainGroupType::Spring) continue;
                pruneDummyLeaves(authoredGroups[gi].mask, gi);
            }
        }
    }

    // Step 3: find the roots inside each physics island. The keyword path uses
    // the keyword mask; the authored path uses the owning group's private mask, so
    // a physics parent from another Cloth piece cannot swallow this group's root.
    struct ChainRootInfo
    {
        uint32_t bone;
        int      srcGroup;  // -1 = keyword path
        int      ringGroup;
        int      ringIndex;
    };
    std::vector<ChainRootInfo> hairRoots, skirtRoots;

    if (!authored)
    {
        for (uint32_t b = 0; b < skel.boneCount; ++b)
        {
            if (!isPhysBone[b]) continue;
            const int32_t parent = skel.parentIndex[b];
            const bool parentIsPhys = parent >= 0 &&
                static_cast<uint32_t>(parent) < skel.boneCount && isPhysBone[parent];
            if (parentIsPhys) continue;

            if (boneClass[b] == 1)
                hairRoots.push_back({ b, -1, 0, -1 });
            else if (boneClass[b] == 2)
                skirtRoots.push_back({ b, -1, ringGroupOf[b], ringIndexOf[b] });
        }
    }
    else
    {
        for (int gi = 0; gi < static_cast<int>(cfg.groups.size()); ++gi)
        {
            const ChainGroupDef& g = cfg.groups[gi];
            if (!g.enabled || g.type == ChainGroupType::Spring) continue;

            const std::vector<bool>& mask = authoredGroups[gi].mask;
            for (uint32_t b = 0; b < skel.boneCount; ++b)
            {
                if (!mask[b]) continue;
                const int32_t parent = skel.parentIndex[b];
                const bool parentInThisGroup =
                    parent >= 0 && static_cast<uint32_t>(parent) < skel.boneCount &&
                    mask[static_cast<uint32_t>(parent)];
                if (parentInThisGroup) continue;

                const ChainRootInfo info{ b, gi, g.ringGroup, g.ringIndex };
                if (g.type == ChainGroupType::Chain) hairRoots.push_back(info);
                else if (g.type == ChainGroupType::Cloth) skirtRoots.push_back(info);
            }
        }
    }

    // Step 4: build hair strands (the DFS forks: one chain per leaf path).
    for (const ChainRootInfo& rootInfo : hairRoots)
    {
        const uint32_t root = rootInfo.bone;
        const std::vector<bool>& traceMask = authored
            ? authoredGroups[rootInfo.srcGroup].mask : isPhysBone;
        std::vector<std::vector<uint32_t>> chains;
        std::vector<uint32_t> current;
        TraceChains(root, skel, traceMask, children, current, chains);

        for (auto& chain : chains)
        {
            if (chain.size() < 2) continue; // need at least 2 bones for a constraint

            Strand strand;
            strand.type = StrandType::Hair;
            strand.particleOffset = static_cast<uint32_t>(data.particles.size());
            strand.particleCount  = static_cast<uint32_t>(chain.size());
            strand.groupIdx       = (rootInfo.srcGroup >= 0)
                                  ? static_cast<uint8_t>(rootInfo.srcGroup) : 0xFF;

            for (size_t i = 0; i < chain.size(); ++i)
            {
                Particle p;
                p.position     = ComputeBindWorldPos(chain[i], skel, poses);
                p.prevPosition = p.position;
                p.restPosition = p.position;
                p.invMass      = (i == 0) ? 0.f : 1.f; // root is pinned
                p.boneIndex    = chain[i];
                data.particles.push_back(p);
            }

            data.strands.push_back(strand);
        }
    }

    // Step 5: build skirt strands, grouped by ring group (Skirt_X_Y's X, or
    // ChainGroupDef::ringGroup) and sorted by ring index (Y) within it. Each ring
    // group forms an independent ring of horizontal / shear constraints.
    struct SkirtChainEntry { int group; int chain; int srcGroup; std::vector<uint32_t> bones; };
    std::vector<SkirtChainEntry> skirtEntries;

    // Generated ordering is counted per authored group / ring group, never from
    // skirtEntries.size(), which would tie auto-order to unrelated earlier pieces.
    std::map<int, int> authoredGroupOrdinal;
    std::map<int, int> autoRingOrdinal;

    // Process explicit ordering before auto-order entries within a piece, so an
    // auto chain appends after the highest authored ringIndex instead of reusing
    // index zero and producing an unstable neighbour order.
    std::sort(skirtRoots.begin(), skirtRoots.end(),
        [](const ChainRootInfo& a, const ChainRootInfo& b)
        {
            if (a.ringGroup != b.ringGroup) return a.ringGroup < b.ringGroup;
            const bool aAuto = a.ringIndex < 0;
            const bool bAuto = b.ringIndex < 0;
            if (aAuto != bAuto) return !aAuto;
            if (a.ringIndex != b.ringIndex) return a.ringIndex < b.ringIndex;
            if (a.srcGroup != b.srcGroup) return a.srcGroup < b.srcGroup;
            return a.bone < b.bone;
        });

    for (const ChainRootInfo& rootInfo : skirtRoots)
    {
        const uint32_t root = rootInfo.bone;
        const std::vector<bool>& traceMask = authored
            ? authoredGroups[rootInfo.srcGroup].mask : isPhysBone;
        std::vector<std::vector<uint32_t>> chains;
        std::vector<uint32_t> current;
        TraceChains(root, skel, traceMask, children, current, chains);
        if (chains.empty()) continue;

        const int baseGroup = rootInfo.ringGroup;
        const int baseChain = rootInfo.ringIndex;
        const int srcGroup  = rootInfo.srcGroup;

        if (authored)
        {
            // Each leaf path traced from the root is one ring member. A single
            // skirt group rooted at the panels' shared parent thus closes the
            // whole ring; ringIndex (if set) offsets the around-ring order.
            for (size_t ci = 0; ci < chains.size(); ++ci)
            {
                if (chains[ci].size() < 2) continue;
                const int ordinal = authoredGroupOrdinal[srcGroup]++;
                const int chainIdx = (baseChain >= 0)
                    ? baseChain + ordinal
                    : autoRingOrdinal[baseGroup]++;
                autoRingOrdinal[baseGroup] =
                    std::max(autoRingOrdinal[baseGroup], chainIdx + 1);
                skirtEntries.push_back({ baseGroup, chainIdx, srcGroup, std::move(chains[ci]) });
            }
        }
        else
        {
            // Keyword path: one chain per Skirt_X_Y root.
            if (chains[0].size() < 2) continue;
            const int chainIdx = (baseChain >= 0)
                ? baseChain
                : autoRingOrdinal[baseGroup]++;
            autoRingOrdinal[baseGroup] =
                std::max(autoRingOrdinal[baseGroup], chainIdx + 1);
            skirtEntries.push_back({ baseGroup, chainIdx, srcGroup, std::move(chains[0]) });
        }
    }

    // Sort by group first, then by chain index within group
    std::sort(skirtEntries.begin(), skirtEntries.end(),
        [](const SkirtChainEntry& a, const SkirtChainEntry& b) {
            if (a.group != b.group) return a.group < b.group;
            return a.chain < b.chain;
        });

    // Build strands and record explicit membership for each Cloth piece. Keeping
    // strand indices (instead of an inferred contiguous range) makes the physics
    // island boundary explicit and robust against future strand insertion/order
    // changes.
    struct SkirtGroup { std::vector<int> strandIndices; };
    std::map<int, SkirtGroup> skirtGroups; // ringGroup -> exact strand members

    for (auto& entry : skirtEntries)
    {
        int strandIdx = static_cast<int>(data.strands.size());
        skirtGroups[entry.group].strandIndices.push_back(strandIdx);

        Strand strand;
        strand.type = StrandType::Skirt;
        strand.particleOffset = static_cast<uint32_t>(data.particles.size());
        strand.particleCount  = static_cast<uint32_t>(entry.bones.size());
        strand.clothPieceId   = entry.group;
        strand.groupIdx       = (entry.srcGroup >= 0)
                              ? static_cast<uint8_t>(entry.srcGroup) : 0xFF;

        for (size_t i = 0; i < entry.bones.size(); ++i)
        {
            Particle p;
            p.position     = ComputeBindWorldPos(entry.bones[i], skel, poses);
            p.prevPosition = p.position;
            p.restPosition = p.position;
            p.invMass      = (i == 0) ? 0.f : 1.f;
            p.boneIndex    = entry.bones[i];
            data.particles.push_back(p);
        }

        data.strands.push_back(strand);
    }

    bool hasSkirt = !skirtGroups.empty();

    // Step 6: Build constraints with color groups

    // Group 0/1: Vertical (structural)  -  even/odd segments
    for (auto& strand : data.strands)
    {
        for (uint32_t i = 0; i + 1 < strand.particleCount; ++i)
        {
            uint32_t pA = strand.particleOffset + i;
            uint32_t pB = strand.particleOffset + i + 1;

            Constraint c;
            c.particleA  = pA;
            c.particleB  = pB;
            c.restLength = Distance3(data.particles[pA].position,
                                     data.particles[pB].position);
            c.stiffness  = 1.0f;
            c.groupIdx   = strand.groupIdx;

            int group = i % 2; // 0 or 1
            data.constraintGroups[group].push_back(c);
        }
    }
    data.groupCount = 2;

    // Group 2/3: Horizontal (skirt only  -  ring neighbours, same segment level)
    // Group 4/5: Shear (skirt only  -  ring neighbours, cross segment)
    // Each constraint group X forms its own ring; chains within a group are
    // connected to their neighbours and the last wraps around to the first.
    if (hasSkirt)
    {
        for (auto& [groupId, sg] : skirtGroups)
        {
            // Collect strands belonging to this group
            std::vector<Strand*> ring;
            ring.reserve(sg.strandIndices.size());
            for (int si : sg.strandIndices)
                ring.push_back(&data.strands[si]);

            int N = static_cast<int>(ring.size());
            if (N < 2) continue;

            for (int ci = 0; ci < N; ++ci)
            {
                Strand* cur  = ring[ci];
                Strand* next = ring[(ci + 1) % N]; // ring wrap

                // Hard runtime guard: never create a horizontal/shear link that
                // crosses a Cloth piece boundary, even if membership bookkeeping
                // is changed incorrectly in a future edit.
                if (cur->clothPieceId != groupId || next->clothPieceId != groupId)
                {
                    LOG_ERROR("ChainPhysics: rejected cross-piece cloth constraint (%d -> %d, expected %d)",
                              cur->clothPieceId, next->clothPieceId, groupId);
                    continue;
                }

                uint32_t segCount = std::min(cur->particleCount, next->particleCount);

                for (uint32_t si = 0; si < segCount; ++si)
                {
                    uint32_t pA = cur->particleOffset + si;
                    uint32_t pB = next->particleOffset + si;
                    float t = (segCount > 1) ? static_cast<float>(si) / (segCount - 1.f) : 0.f;

                    // Horizontal constraint
                    Constraint h;
                    h.particleA  = pA;
                    h.particleB  = pB;
                    h.restLength = Distance3(data.particles[pA].position,
                                             data.particles[pB].position);
                    h.stiffness  = 0.8f;
                    h.segmentT   = t;
                    h.groupIdx   = cur->groupIdx;
                    data.constraintGroups[2 + (ci % 2)].push_back(h);

                    // Shear constraint (cross-level)
                    if (si + 1 < segCount)
                    {
                        uint32_t pC = next->particleOffset + si + 1;
                        float tShear = (segCount > 1) ? (si + 0.5f) / (segCount - 1.f) : 0.f;

                        Constraint s;
                        s.particleA  = pA;
                        s.particleB  = pC;
                        s.restLength = Distance3(data.particles[pA].position,
                                                 data.particles[pC].position);
                        s.stiffness  = 0.5f;
                        s.segmentT   = tShear;
                        s.groupIdx   = cur->groupIdx;
                        data.constraintGroups[4 + (ci % 2)].push_back(s);
                    }
                }
            }
        }
        data.groupCount = 6;
    }

    // Step 7: build spring bones (single jiggle bones). Pairing comes from
    // springPairId[] (keyword path: chest bones sharing a parent; authored: Spring
    // groups sharing pairGroupId). Two or more bones sharing a pair value collapse
    // to a virtual midpoint root; every other one is an independent spring bone.
    std::vector<uint32_t> springBoneIdx;
    for (uint32_t b = 0; b < skel.boneCount; ++b)
        if (boneClass[b] == 3) springBoneIdx.push_back(b);

    // Group by pair id (>= 0 only). std::map for deterministic iteration order.
    std::map<int, std::vector<uint32_t>> springByPair;
    for (uint32_t b : springBoneIdx)
        if (springPairId[b] >= 0) springByPair[springPairId[b]].push_back(b);

    // Bones that will live under a virtual root (pair groups of size >= 2).
    std::unordered_set<uint32_t> pairedWithVirtual;
    for (auto& [pid, group] : springByPair)
        if (group.size() >= 2)
            pairedWithVirtual.insert(group.begin(), group.end());

    // Create virtual roots first, then their children.
    for (auto& [pid, group] : springByPair)
    {
        if (group.size() < 2) continue; // lone pair member -> independent below

        // Virtual root's parent bone (for its world xform) = parent of the first
        // group member (the shared parent in the keyword chest-pair case).
        const int32_t parentIdx = skel.parentIndex[group[0]];

        // Compute midpoint of all spring bones in this group.
        XMFLOAT3 midpoint = { 0.f, 0.f, 0.f };
        for (uint32_t cb : group)
        {
            XMFLOAT3 wp = ComputeBindWorldPos(cb, skel, poses);
            midpoint.x += wp.x; midpoint.y += wp.y; midpoint.z += wp.z;
        }
        float invN = 1.0f / static_cast<float>(group.size());
        midpoint.x *= invN; midpoint.y *= invN; midpoint.z *= invN;

        // Create virtual root spring bone.
        int32_t rootIdx = static_cast<int32_t>(data.springBones.size());
        SpringBone root;
        root.boneIndex      = ~0u;
        root.restLocalPos   = { 0.f, 0.f, 0.f };
        root.currentWorldPos = midpoint;
        root.velocity       = { 0.f, 0.f, 0.f };
        root.isVirtual      = true;
        root.parentBoneIndex = (parentIdx >= 0) ? static_cast<uint32_t>(parentIdx) : 0u;
        root.groupIdx       = (boneGroup[group[0]] >= 0)
                            ? static_cast<uint8_t>(boneGroup[group[0]]) : 0xFF;
        data.springBones.push_back(root);

        LOG_INFO("ChainPhysics: created virtual Chest_Root (parent bone %d) for %zu chest bones",
                 parentIdx, group.size());

        // Create child spring bones linked to the virtual root.
        for (uint32_t cb : group)
        {
            XMFLOAT3 wp = ComputeBindWorldPos(cb, skel, poses);
            SpringBone child;
            child.boneIndex      = cb;
            child.restLocalPos   = { poses[cb].pos.x, poses[cb].pos.y, poses[cb].pos.z };
            child.currentWorldPos = wp;
            child.velocity       = { 0.f, 0.f, 0.f };
            child.parentSpringIdx = rootIdx;
            child.groupIdx        = (boneGroup[cb] >= 0)
                                  ? static_cast<uint8_t>(boneGroup[cb]) : 0xFF;
            data.springBones.push_back(child);
        }
    }

    // Independent spring bones (lone pair members + unpaired jiggle bones).
    for (uint32_t b : springBoneIdx)
    {
        if (pairedWithVirtual.count(b)) continue; // already a virtual-root child

        SpringBone sb;
        sb.boneIndex      = b;
        sb.restLocalPos   = { poses[b].pos.x, poses[b].pos.y, poses[b].pos.z };
        sb.currentWorldPos = ComputeBindWorldPos(b, skel, poses);
        sb.velocity       = { 0.f, 0.f, 0.f };
        sb.groupIdx       = (boneGroup[b] >= 0)
                          ? static_cast<uint8_t>(boneGroup[b]) : 0xFF;
        data.springBones.push_back(sb);
    }

    // Divergence safeguard bound: 2x the strand's rest extent (sum of segment
    // lengths) + 0.05, a generous swing margin that still caps a runaway sim far
    // below screen size. Computed from the init pose; a bad init pose makes the
    // clamp trigger (and log) at once, which is the wanted signal.
    for (auto& strand : data.strands)
    {
        float ext = 0.f;
        for (uint32_t i = 0; i + 1 < strand.particleCount; ++i)
            ext += Distance3(data.particles[strand.particleOffset + i].position,
                             data.particles[strand.particleOffset + i + 1].position);
        const float reach = ext * 2.0f + 0.05f;
        strand.maxReachSq = reach * reach;
    }

    // Root-bone write eligibility (KawaiiPhysics parity): WriteBonePoses rotates a
    // strand's root bone toward particle 1 only when this strand is the SOLE owner
    // of that root. Forked chains duplicate the shared root into every leaf strand,
    // and an authored Cloth group rooted at the panels' shared parent gives ALL
    // ring strands one root, where a last-writer-wins rotation would rigidly tilt
    // the whole piece toward the last panel.
    {
        std::unordered_map<uint32_t, int> rootUse;
        for (const auto& strand : data.strands)
            ++rootUse[data.particles[strand.particleOffset].boneIndex];
        for (auto& strand : data.strands)
            strand.rootSolo = strand.particleCount >= 2 &&
                rootUse[data.particles[strand.particleOffset].boneIndex] == 1;
    }

    // One writer per bone (see Particle::writesBone). A fork family shares its
    // prefix across every leaf strand; the LONGEST strand through a bone owns
    // its pose write, ties going to trace order, so a 30-link hair strand's top
    // links follow that strand's own solver rather than a 7-particle bow branch
    // traced after it. Cloth strands never share non-root bones, so this is a
    // no-op for skirt rings; the shared-root case stays with rootSolo above.
    {
        std::vector<int> ownerStrand(skel.boneCount, -1);
        for (int si = 0; si < static_cast<int>(data.strands.size()); ++si)
        {
            const Strand& s = data.strands[si];
            for (uint32_t i = 0; i + 1 < s.particleCount; ++i)
            {
                const uint32_t bone = data.particles[s.particleOffset + i].boneIndex;
                if (bone >= skel.boneCount) continue;
                const int cur = ownerStrand[bone];
                if (cur < 0 || data.strands[cur].particleCount < s.particleCount)
                    ownerStrand[bone] = si;
            }
        }
        for (int si = 0; si < static_cast<int>(data.strands.size()); ++si)
        {
            const Strand& s = data.strands[si];
            for (uint32_t i = 0; i + 1 < s.particleCount; ++i)
            {
                Particle& p = data.particles[s.particleOffset + i];
                p.writesBone = (p.boneIndex < skel.boneCount) &&
                               ownerStrand[p.boneIndex] == si;
            }
        }
    }

    // Pre-size the solver scratch ONCE. Islands of this entity run as concurrent
    // jobs and each touches only its own particle range of these buffers; a
    // per-job resize() would race on the vector itself.
    data.poseStartScratch.resize(data.particles.size());
    data.collisionPushScratch.resize(data.particles.size());
    data.collisionWeightScratch.resize(data.particles.size());
    data.prevPosSnapshotScratch.resize(data.particles.size());
    data.localBlendScratch.resize(data.particles.size());
    data.windOffsetScratch.resize(data.particles.size());
    for (int g = 0; g < kMaxGroups; ++g)
        data.constraintStiffScratch[g].resize(data.constraintGroups[g].size());

    BuildIslands(data, skel);

    data.initialized = true;

    // Log summary
    uint32_t totalConstraints = 0;
    for (int g = 0; g < data.groupCount; ++g)
        totalConstraints += static_cast<uint32_t>(data.constraintGroups[g].size());

    LOG_INFO("ChainPhysics: entity %u  -  %zu strands, %zu cloth pieces, %u particles, %u constraints (%d solver colors), %zu spring bones, %zu islands (%zu tasks)",
             e, data.strands.size(),
             skirtGroups.size(),
             static_cast<uint32_t>(data.particles.size()),
             totalConstraints, data.groupCount,
             data.springBones.size(),
             data.islands.size(), data.islandTasks.size());
}

// ==== BuildIslands: partition strands into data-disjoint parallel islands ====
// Island boundaries follow the strand build order, contiguous by construction:
// hair strands are appended per root (a fork family shares its root bone), then
// cloth strands sorted by piece id. Constraints are pushed in the same strand /
// piece order, so each island owns one contiguous run of every color group (a
// defensive stable partition keeps it so).
//
// Islands whose pose-FK ancestor chain passes through a bone WRITTEN by another
// island (authored mode: a group rooted under another group's subtree) merge
// into one serial task, which avoids a data race on poses[] and keeps the
// serial write order.
void ChainPhysicsSystem::BuildIslands(EntityData& data, const SkeletonAsset& skel)
{
    data.islands.clear();
    data.islandTasks.clear();

    // ---- 1) Contiguous strand runs -> islands ----
    const uint32_t S = static_cast<uint32_t>(data.strands.size());
    uint32_t s = 0;
    while (s < S)
    {
        Island isl;
        isl.strandBegin = s;
        const Strand& first = data.strands[s];
        if (first.type == StrandType::Hair)
        {
            // One island per hair root: forked strands duplicate the root bone.
            const uint32_t rootBone = data.particles[first.particleOffset].boneIndex;
            do { ++s; } while (s < S && data.strands[s].type == StrandType::Hair &&
                data.particles[data.strands[s].particleOffset].boneIndex == rootBone);
        }
        else
        {
            // One island per cloth piece: ring/shear constraints couple every
            // strand sharing a clothPieceId.
            const int piece = first.clothPieceId;
            do { ++s; } while (s < S && data.strands[s].type == StrandType::Skirt &&
                data.strands[s].clothPieceId == piece);
        }
        isl.strandEnd = s;
        isl.particleBegin = data.strands[isl.strandBegin].particleOffset;
        const Strand& last = data.strands[isl.strandEnd - 1];
        isl.particleEnd = last.particleOffset + last.particleCount;
        data.islands.push_back(isl);
    }

    // Spring bones form their own (strand-less) island so jiggle can overlap
    // cloth solving; the conflict merge below re-serializes it if needed.
    if (!data.springBones.empty())
    {
        Island isl;
        isl.strandBegin = isl.strandEnd = S;
        isl.particleBegin = isl.particleEnd =
            static_cast<uint32_t>(data.particles.size());
        isl.springs = true;
        data.islands.push_back(isl);
    }

    const int islandCount = static_cast<int>(data.islands.size());
    if (islandCount == 0) return;

    // ---- 2) Per-color-group constraint ranges ----
    std::vector<int> pIsland(data.particles.size(), -1);
    for (int ii = 0; ii < islandCount; ++ii)
        for (uint32_t p = data.islands[ii].particleBegin; p < data.islands[ii].particleEnd; ++p)
            pIsland[p] = ii;

    // Parallelism invariant: a constraint's endpoints must live in ONE island, since
    // the ranges below bucket by particleA only. A cross-island link forces both
    // islands into one serial task instead of racing on particleB from two workers.
    std::vector<std::pair<int, int>> forcedUnions;

    for (int g = 0; g < data.groupCount; ++g)
    {
        auto& v = data.constraintGroups[g];
        // Defensive: entries are already grouped by island via build order; the
        // stable partition keeps intra-island solve order if that ever changes.
        std::stable_sort(v.begin(), v.end(),
            [&](const Constraint& a, const Constraint& b)
            { return pIsland[a.particleA] < pIsland[b.particleA]; });

        for (const auto& c : v)
            if (pIsland[c.particleA] != pIsland[c.particleB])
                forcedUnions.emplace_back(pIsland[c.particleA], pIsland[c.particleB]);

        uint32_t idx = 0;
        for (int ii = 0; ii < islandCount; ++ii)
        {
            data.islands[ii].constraintBegin[g] = idx;
            while (idx < v.size() && pIsland[v[idx].particleA] == ii) ++idx;
            data.islands[ii].constraintEnd[g] = idx;
        }
    }

    // ---- 3) FK-read vs pose-write conflict merge ----
    // writeMask = bones this island's pose write-back mutates (strand bones in
    // WriteBonePoses' range; spring bones for the spring island). readMask =
    // every ancestor walked by that write-back's FK (ComputeWorldTransform /
    // ComputeBindWorldPos). read(i) AND write(j) non-empty, i != j => i and j
    // share a task.
    const uint32_t words = (skel.boneCount + 63u) / 64u;
    std::vector<uint64_t> writeMask(static_cast<size_t>(islandCount) * words, 0ull);
    std::vector<uint64_t> readMask (static_cast<size_t>(islandCount) * words, 0ull);
    auto mark = [&](std::vector<uint64_t>& m, int ii, uint32_t bone)
    { m[static_cast<size_t>(ii) * words + bone / 64u] |= 1ull << (bone % 64u); };
    auto markAncestors = [&](std::vector<uint64_t>& m, int ii, uint32_t bone)
    {
        int32_t cur = static_cast<int32_t>(bone);
        uint32_t depth = 0;
        while (cur >= 0 && static_cast<uint32_t>(cur) < skel.boneCount && depth++ < skel.boneCount)
        {
            mark(m, ii, static_cast<uint32_t>(cur));
            cur = skel.parentIndex[cur];
        }
    };

    for (int ii = 0; ii < islandCount; ++ii)
    {
        const Island& isl = data.islands[ii];
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
        {
            const Strand& strand = data.strands[si];
            const uint32_t iBegin = strand.rootSolo ? 0u : 1u;
            for (uint32_t i = iBegin; i + 1 < strand.particleCount; ++i)
            {
                const uint32_t bone = data.particles[strand.particleOffset + i].boneIndex;
                if (bone >= skel.boneCount) continue;
                mark(writeMask, ii, bone);
                markAncestors(readMask, ii, bone);
            }
            // WriteBonePoses also reads the strand TIP bone (poses[childBone].pos for
            // the last aim): mark it so an island that writes .pos cannot slip past the
            // merge (every phase-2 writer writes .rot only).
            if (strand.particleCount >= 2)
            {
                const uint32_t tip = data.particles[strand.particleOffset +
                                                    strand.particleCount - 1].boneIndex;
                if (tip < skel.boneCount) mark(readMask, ii, tip);
            }
        }
        if (isl.springs)
        {
            for (const auto& sb : data.springBones)
            {
                if (sb.isVirtual || sb.boneIndex >= skel.boneCount) continue;
                markAncestors(readMask, ii, sb.boneIndex);
                if (skel.parentIndex[sb.boneIndex] >= 0)
                    mark(writeMask, ii, sb.boneIndex);
            }
        }
    }

    std::vector<int> taskOf(islandCount);
    for (int ii = 0; ii < islandCount; ++ii) taskOf[ii] = ii;
    auto findRoot = [&](int i) { while (taskOf[i] != i) i = taskOf[i] = taskOf[taskOf[i]]; return i; };

    for (const auto& [a, b] : forcedUnions)
    {
        if (a < 0 || b < 0) continue;
        const int ra = findRoot(a), rb = findRoot(b);
        if (ra != rb) taskOf[std::max(ra, rb)] = std::min(ra, rb);
    }

    for (int i = 0; i < islandCount; ++i)
        for (int j = 0; j < islandCount; ++j)
        {
            if (i == j) continue;
            bool overlap = false;
            for (uint32_t w = 0; w < words && !overlap; ++w)
                overlap = (readMask[static_cast<size_t>(i) * words + w] &
                           writeMask[static_cast<size_t>(j) * words + w]) != 0ull;
            if (overlap)
            {
                const int ri = findRoot(i), rj = findRoot(j);
                if (ri != rj) taskOf[std::max(ri, rj)] = std::min(ri, rj);
            }
        }

    // Emit tasks with member islands in ascending order (preserves the serial
    // strand-write order inside a merged task).
    std::map<int, std::vector<int>> tasks;
    for (int ii = 0; ii < islandCount; ++ii)
        tasks[findRoot(ii)].push_back(ii);
    for (auto& [root, members] : tasks)
        data.islandTasks.push_back(std::move(members));
}

// ==== ApplyRestLimit: KawaiiPhysics LimitAngle, as an interleaved constraint ====
// Bounds each bone's simulated direction to a cone around its ANIMATED rest
// direction. localStiffness is only a spring: a hard impulse (dash, spin,
// landing) still drags the chain arbitrarily far before decaying back. This is
// the hard bound, and it is what makes a group safe to exclude from collision.
//
// The cone is measured against the animation pose, so it rotates with the
// character: no frozen model-space reference goes stale after a turn. The
// correction ROTATES the parent->child offset about the parent, so |offset| is
// preserved bit-for-bit and the vertical PBD link is never disturbed.
void ChainPhysicsSystem::ApplyRestLimit(
    EntityData& data, const Island& isl,
    const ChainPhysicsComponent& cfg,
    const SkeletonAsset& skel,
    float subAlpha, bool applyVelocityResponse,
    const WorldCapsule* capsules, int capsuleCount, float skin)
{
    auto cachedBonePos = [&](uint32_t bi) -> XMFLOAT3 {
        if (bi >= skel.boneCount) return { 0.f, 0.f, 0.f };
        const auto& m = data.boneWorldMatCache[bi];
        return { m._41, m._42, m._43 };
    };
    auto grpOvr = [&](uint8_t gi) -> const ChainGroupDef* {
        return (gi < cfg.groups.size() && cfg.groups[gi].ovrEnabled) ? &cfg.groups[gi] : nullptr;
    };
    // Rest positions interpolate from last frame's pose to this frame's across
    // the substeps, matching the kinematic roots / colliders / pull targets.
    auto lerpRest = [&](uint32_t idx, const XMFLOAT3& now) -> XMFLOAT3 {
        const XMFLOAT3& was = data.poseStartScratch[idx];
        return { was.x + (now.x - was.x) * subAlpha,
                 was.y + (now.y - was.y) * subAlpha,
                 was.z + (now.z - was.z) * subAlpha };
    };

    for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
    {
        const Strand& strand = data.strands[si];
        if (strand.particleCount < 2) continue;
        const ChainGroupDef* o = grpOvr(strand.groupIdx);
        const bool isSkirt = (strand.type == StrandType::Skirt);
        const float limDeg = o ? o->ovrLimitAngle
                               : (isSkirt ? cfg.skirtLimitAngle : cfg.limitAngle);
        if (limDeg <= 0.f || limDeg >= 180.f) continue;   // 0 = unlimited (default)
        const float tipScale = std::max(o ? o->ovrLimitTipScale
                                          : (isSkirt ? cfg.skirtLimitTipScale
                                                     : cfg.limitTipScale), 0.f);

        const float chainLen = static_cast<float>(strand.particleCount);
        for (uint32_t i = 1; i < strand.particleCount; ++i)
        {
            const uint32_t pIdx = strand.particleOffset + i;
            Particle& p = data.particles[pIdx];
            const Particle& parent = data.particles[pIdx - 1];

            const XMFLOAT3 rc = lerpRest(pIdx,     cachedBonePos(p.boneIndex));
            const XMFLOAT3 rp = lerpRest(pIdx - 1, cachedBonePos(parent.boneIndex));
            XMVECTOR rest = XMVectorSet(rc.x - rp.x, rc.y - rp.y, rc.z - rp.z, 0.f);
            if (XMVectorGetX(XMVector3LengthSq(rest)) < 1e-12f) continue;
            rest = XMVector3Normalize(rest);

            const XMVECTOR cur = XMVectorSet(p.position.x - parent.position.x,
                                             p.position.y - parent.position.y,
                                             p.position.z - parent.position.z, 0.f);
            const float len = XMVectorGetX(XMVector3Length(cur));
            if (len < 1e-6f) continue;
            const XMVECTOR dir = XMVectorScale(cur, 1.f / len);

            // The cone widens (tipScale > 1) or narrows (< 1) along the chain. The
            // local pull's own falloff is (1-t)^2 (near-full at the root, zero at the
            // tip), so a UNIFORM cone is only ever reached by the tip, which reads as
            // "the tip went rigid and the root did nothing". Widening toward the tip
            // makes the limit a safety bound instead of a second, competing shape
            // control.
            const float t = static_cast<float>(i) / (chainLen - 1.f);
            float half = limDeg * (1.f + t * (tipScale - 1.f));
            half = std::min(std::max(half, 0.f), 179.f);
            const float cosLim = std::cosf(half * 0.01745329252f);

            float d = XMVectorGetX(XMVector3Dot(dir, rest));
            d = std::min(std::max(d, -1.f), 1.f);
            if (d >= cosLim) continue;              // already inside the cone

            // Slerp onto the cone boundary: rest*cos(lim) + perp*sin(lim).
            XMVECTOR perp = XMVectorSubtract(dir, XMVectorScale(rest, d));
            float pl = XMVectorGetX(XMVector3Length(perp));
            if (pl < 1e-6f)
            {
                // Exactly anti-parallel to rest: no unique swing plane, so any
                // perpendicular is as good as another.
                const XMVECTOR up = (std::fabsf(XMVectorGetY(rest)) < 0.99f)
                    ? XMVectorSet(0.f, 1.f, 0.f, 0.f)
                    : XMVectorSet(1.f, 0.f, 0.f, 0.f);
                perp = XMVector3Cross(rest, up);
                pl = XMVectorGetX(XMVector3Length(perp));
                if (pl < 1e-6f) continue;
            }
            perp = XMVectorScale(perp, 1.f / pl);

            const float sinLim = std::sqrtf(std::max(0.f, 1.f - cosLim * cosLim));
            XMFLOAT3 nd;
            XMStoreFloat3(&nd, XMVectorAdd(XMVectorScale(rest, cosLim),
                                           XMVectorScale(perp, sinLim)));
            const XMFLOAT3 newPos = { parent.position.x + nd.x * len,
                                      parent.position.y + nd.y * len,
                                      parent.position.z + nd.z * len };

            // Infeasible-cone guard: when the boundary point sits inside a collider's
            // contact field, cone clamp and collision have an empty intersection, and
            // snapping here would hand collision a particle to eject again next pass,
            // forever. Skip the clamp and let collision own the segment; projecting
            // newPos out instead would change the segment length this function exists
            // to preserve.
            if (capsules)
            {
                bool infeasible = false;
                for (int c = 0; c < capsuleCount && !infeasible; ++c)
                {
                    const WorldCapsule& cap = capsules[c];
                    const float r = (cap.radius + p.radius) * skin;
                    const float abx = cap.b.x - cap.a.x, aby = cap.b.y - cap.a.y,
                                abz = cap.b.z - cap.a.z;
                    const float apx = newPos.x - cap.a.x, apy = newPos.y - cap.a.y,
                                apz = newPos.z - cap.a.z;
                    const float abDot = abx*abx + aby*aby + abz*abz;
                    const float ct = (abDot > 1e-8f)
                        ? std::max(0.f, std::min(1.f,
                              (apx*abx + apy*aby + apz*abz) / abDot)) : 0.f;
                    const float ddx = newPos.x - (cap.a.x + ct * abx);
                    const float ddy = newPos.y - (cap.a.y + ct * aby);
                    const float ddz = newPos.z - (cap.a.z + ct * abz);
                    infeasible = (ddx*ddx + ddy*ddy + ddz*ddz) < r * r;
                }
                if (infeasible) continue;
            }

            if (applyVelocityResponse)
            {
                // Cancel ONLY the outward component, same shape as the collision
                // velResponse. Translating prevPosition with the particle would
                // let it keep ramming the cone and release the stored energy as a
                // burst once the motion stops; zeroing it outright would kill all
                // motion whenever the chain sits at its limit. Tangential slide
                // along the cone is preserved.
                const float nx = newPos.x - p.position.x;
                const float ny = newPos.y - p.position.y;
                const float nz = newPos.z - p.position.z;
                const float nl = std::sqrtf(nx * nx + ny * ny + nz * nz);
                float vx = p.position.x - p.prevPosition.x;
                float vy = p.position.y - p.prevPosition.y;
                float vz = p.position.z - p.prevPosition.z;
                if (nl > 1e-8f)
                {
                    const float ix = nx / nl, iy = ny / nl, iz = nz / nl; // inward unit
                    const float vn = vx * ix + vy * iy + vz * iz;
                    if (vn < 0.f) { vx -= vn * ix; vy -= vn * iy; vz -= vn * iz; }
                }
                p.prevPosition = { newPos.x - vx, newPos.y - vy, newPos.z - vz };
            }
            p.position = newPos;
        }
    }
}

// ==== IntegrateStrand: Verlet integration of one strand ====
void ChainPhysicsSystem::IntegrateStrand(
    EntityData& data, const Strand& strand, float dt,
    float damping, const XMFLOAT3& gravity, float maxVelocity)
{
    float dt2 = dt * dt;
    // A PREVIEW-lane poke's acceleration, resolved per pass in serial phase 1
    // (PrepareExternalImpulses) and added beside the gravity term. Null on every
    // main-lane job and on a preview pass with nothing pending: the authored
    // integration, byte for byte (the term is added on its own, never folded
    // into gravity).
    const XMFLOAT3* ext = (data.extAccelActive && data.extAccel.size() >=
                           static_cast<size_t>(strand.particleOffset) + strand.particleCount)
        ? data.extAccel.data() + strand.particleOffset : nullptr;
    for (uint32_t i = 0; i < strand.particleCount; ++i)
    {
        Particle& p = data.particles[strand.particleOffset + i];
        if (p.invMass == 0.f) continue;

        float vx = (p.position.x - p.prevPosition.x) * damping;
        float vy = (p.position.y - p.prevPosition.y) * damping;
        float vz = (p.position.z - p.prevPosition.z) * damping;

        // Clamp velocity to prevent explosive motion
        if (maxVelocity > 0.f)
        {
            float vLen2 = vx * vx + vy * vy + vz * vz;
            float maxV2 = maxVelocity * maxVelocity;
            if (vLen2 > maxV2)
            {
                float scale = maxVelocity / std::sqrtf(vLen2);
                vx *= scale;
                vy *= scale;
                vz *= scale;
            }
        }

        XMFLOAT3 newPos;
        newPos.x = p.position.x + vx + gravity.x * dt2;
        newPos.y = p.position.y + vy + gravity.y * dt2;
        newPos.z = p.position.z + vz + gravity.z * dt2;
        if (ext)
        {
            newPos.x += ext[i].x * dt2;
            newPos.y += ext[i].y * dt2;
            newPos.z += ext[i].z * dt2;
        }

        p.prevPosition = p.position;
        p.position     = newPos;
    }
}

// ==== WriteBonePoses: particle positions -> bone local rotations ====
// Works in the PARENT bone's local frame: the simulated direction (world) goes
// into the parent's local space, the bind direction is the child's local rest
// offset, and the swing from the animated to the simulated direction is
// composed onto the animated rotation.
// i == 0 (chain root, rootSolo strands only) is written the same way
// (KawaiiPhysics ApplySimulateResult: FindBetweenVectors(pose, sim) *
// PoseRotation); without it the top ring of a skirt / jacket follows
// animation rigidly and a 2-bone chain shows no simulation at all.
void ChainPhysicsSystem::WriteBonePoses(
    EntityData& data, const Island& isl,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    // Owners first. A bone above a fork is written only by the longest strand
    // through it (Particle::writesBone); a shorter branch strand skips those
    // bones but composes its OWN bones' parent transform from them, so the
    // owner's rotation has to be in poses[] before the branch reads it. Strand
    // order within an island is trace order (a branch can precede the long
    // path it hangs off), hence the sort. Islands hold a handful of strands.
    uint32_t order[64];
    uint32_t orderCount = 0;
    std::vector<uint32_t> orderHeap;
    uint32_t* ord = order;
    {
        const uint32_t n = isl.strandEnd - isl.strandBegin;
        if (n > 64) { orderHeap.resize(n); ord = orderHeap.data(); }
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            ord[orderCount++] = si;
        std::stable_sort(ord, ord + orderCount, [&](uint32_t a, uint32_t b)
        {
            return data.strands[a].particleCount > data.strands[b].particleCount;
        });
    }

    for (uint32_t oi = 0; oi < orderCount; ++oi)
    {
        const uint32_t si = ord[oi];
        const Strand& strand = data.strands[si];
        const uint32_t iBegin = strand.rootSolo ? 0u : 1u;

        // Parent world transform, carried DOWN the strand instead of rebuilt from
        // the root per bone: a strand IS a parent->child chain, so bone[i]'s parent
        // is bone[i-1], the bone the previous iteration finished. A per-bone
        // ComputeWorldTransform is O(depth) and a top profile cost; the carry is O(1).
        //
        // The carry advances at the TOP of the iteration: several `continue` paths
        // skip the pose write, and the next bone's parent transform must still
        // include this bone.
        //
        // The chain assumption is CHECKED: if the previous bone is not this bone's
        // parent, the full walk runs instead (correct, just slower).
        XMMATRIX carry{};
        bool     carryValid = false;
        uint32_t prevBone   = 0;

        for (uint32_t i = iBegin; i + 1 < strand.particleCount; ++i)
        {
            uint32_t pIdx      = strand.particleOffset + i;
            uint32_t childIdx  = pIdx + 1;
            uint32_t boneIdx   = data.particles[pIdx].boneIndex;
            uint32_t childBone = data.particles[childIdx].boneIndex;

            const int32_t parentOfThis = (boneIdx < skel.boneCount)
                ? skel.parentIndex[boneIdx] : -1;
            if (carryValid && parentOfThis >= 0
                && static_cast<uint32_t>(parentOfThis) == prevBone)
            {
                // carry currently holds world(prevBone's parent); compose the
                // previous bone's own (possibly just-updated) local pose onto it
                // to get world(prevBone) == world(this bone's parent).
                carry = LocalPoseToMat(poses[prevBone]) * carry;
            }
            else
            {
                carry = (parentOfThis >= 0)
                    ? ComputeWorldTransform(static_cast<uint32_t>(parentOfThis), poses, skel)
                    : XMMatrixIdentity();
                carryValid = true;
            }
            prevBone = boneIdx;

            // Another (longer) strand owns this bone's write (Particle::writesBone);
            // the carry above already advanced past it.
            if (!data.particles[pIdx].writesBone) continue;

            XMVECTOR bindLocal = XMVectorSet(
                poses[childBone].pos.x,
                poses[childBone].pos.y,
                poses[childBone].pos.z, 0.f);
            float bindLen = XMVectorGetX(XMVector3Length(bindLocal));
            if (bindLen < 1e-6f) continue;
            bindLocal = XMVectorScale(bindLocal, 1.f / bindLen);

            XMFLOAT3 from = data.particles[pIdx].position;
            XMFLOAT3 to   = data.particles[childIdx].position;
            XMVECTOR simWorld = XMVector3Normalize(XMVectorSubtract(
                XMLoadFloat3(&to), XMLoadFloat3(&from)));

            XMMATRIX parentInv = XMMatrixInverse(nullptr, carry);

            XMVECTOR simLocal = XMVector3Normalize(
                XMVector3TransformNormal(simWorld, parentInv));

            const XMVECTOR localRot = ChainPhysicsMath::AimAnimatedRotation(
                bindLocal, simLocal, XMLoadFloat4(&poses[boneIdx].rot));
            XMStoreFloat4(&poses[boneIdx].rot, localRot);
        }
    }
}

// ==== SimulateSpringBones: spring-damper dynamics for jiggle bones ====
// Each spring bone tracks a "goal" position (where animation wants it) and a
// "current" simulated position. The spring force pulls current toward goal,
// while inertia makes it overshoot and oscillate.
//
// ODE:  acceleration = (-k*(pos - goal) - d*vel + gravity) / mass
// Integration: semi-implicit Euler (velocity first, then position).
void ChainPhysicsSystem::SimulateSpringBones(
    EntityData& data, const ChainPhysicsComponent& cfg,
    float dt, AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel,
    const XMFLOAT3* gravityDown,
    const WorldKick* worldKick)
{
    // Simulates one spring bone toward goalWorld with the given parameters, then
    // clamps its displacement to maxDisp (halving the velocity when it clamps).
    auto simulateOne = [&](SpringBone& sb, const XMFLOAT3& goalWorld,
                           float k, float d, float mass, float grav, float maxDisp)
    {
        float dx = sb.currentWorldPos.x - goalWorld.x;
        float dy = sb.currentWorldPos.y - goalWorld.y;
        float dz = sb.currentWorldPos.z - goalWorld.z;

        float fx = -k * dx - d * sb.velocity.x;
        float fy = -k * dy - d * sb.velocity.y + grav;
        float fz = -k * dz - d * sb.velocity.z;
        // The preview scene's gravity VECTOR (preview lane only; null on the main
        // lane, which keeps the scalar-Y path above).
        if (gravityDown)
        {
            fx = -k * dx - d * sb.velocity.x + gravityDown->x * -grav;
            fy = -k * dy - d * sb.velocity.y + gravityDown->y * -grav;
            fz = -k * dz - d * sb.velocity.z + gravityDown->z * -grav;
        }

        float invMass = 1.0f / std::max(mass, 0.01f);
        sb.velocity.x += fx * invMass * dt;
        sb.velocity.y += fy * invMass * dt;
        sb.velocity.z += fz * invMass * dt;

        sb.currentWorldPos.x += sb.velocity.x * dt;
        sb.currentWorldPos.y += sb.velocity.y * dt;
        sb.currentWorldPos.z += sb.velocity.z * dt;

        dx = sb.currentWorldPos.x - goalWorld.x;
        dy = sb.currentWorldPos.y - goalWorld.y;
        dz = sb.currentWorldPos.z - goalWorld.z;
        float dispLen = std::sqrtf(dx * dx + dy * dy + dz * dz);
        float md = std::max(maxDisp, 0.01f);
        if (dispLen > md)
        {
            float scale = md / dispLen;
            sb.currentWorldPos.x = goalWorld.x + dx * scale;
            sb.currentWorldPos.y = goalWorld.y + dy * scale;
            sb.currentWorldPos.z = goalWorld.z + dz * scale;
            sb.velocity.x *= 0.5f;
            sb.velocity.y *= 0.5f;
            sb.velocity.z *= 0.5f;
        }
    };

    // Per-group override (or nullptr -> use the global spring params).
    auto grpOvr = [&](uint8_t gi) -> const ChainGroupDef* {
        return (gi < cfg.groups.size() && cfg.groups[gi].ovrEnabled) ? &cfg.groups[gi] : nullptr;
    };

    // World-motion inertia (see FeltKick): conserve each spring's WORLD
    // velocity across this pass's entity motion before the spring-damper step,
    // so a turn / start / stop jiggles it. Virtual roots too (their lag is what
    // their children's goals follow). Frozen springs are reset below anyway.
    if (worldKick)
        for (SpringBone& sb : data.springBones)
        {
            if (sb.frozen) continue;
            const ChainGroupDef* gd = sb.groupIdx < cfg.groups.size()
                                    ? &cfg.groups[sb.groupIdx] : nullptr;
            FeltKick k;
            if (!k.Resolve(worldKick->rot, worldKick->pos, worldKick->dt,
                           worldKick->prevRot, worldKick->prevPos, worldKick->prevDt,
                           EffectiveWorldInertia(gd, cfg, false),
                           EffectiveWorldInertia(gd, cfg, true)))
                continue;
            const XMVECTOR x = XMLoadFloat3(&sb.currentWorldPos);
            XMStoreFloat3(&sb.velocity,
                          XMVectorAdd(k.Turn(XMLoadFloat3(&sb.velocity)), k.Delta(x)));
        }

    // Pass 1: virtual roots; goal = midpoint of the children's animated positions.
    for (size_t i = 0; i < data.springBones.size(); ++i)
    {
        SpringBone& sb = data.springBones[i];
        if (!sb.isVirtual) continue;

        XMFLOAT3 goal = { 0.f, 0.f, 0.f };
        int childCount = 0;
        for (auto& child : data.springBones)
        {
            if (child.parentSpringIdx == static_cast<int32_t>(i))
            {
                XMFLOAT3 wp = ComputeBindWorldPos(child.boneIndex, skel, poses);
                goal.x += wp.x; goal.y += wp.y; goal.z += wp.z;
                ++childCount;
            }
        }
        if (childCount > 0)
        {
            float inv = 1.0f / static_cast<float>(childCount);
            goal.x *= inv; goal.y *= inv; goal.z *= inv;
        }

        // Stash this frame's animated midpoint so Pass 2 children can express their
        // goal relative to it in the CURRENT facing (not a frozen bind-pose offset).
        sb.animMidpoint = goal;

        // A spring frozen by a preview-lane solo is held AT its goal (zero lag ->
        // WriteSpringBonePoses writes no jiggle). The main lane never freezes.
        if (sb.frozen) { sb.currentWorldPos = goal; sb.velocity = { 0.f, 0.f, 0.f }; continue; }

        const ChainGroupDef* o = grpOvr(sb.groupIdx);
        simulateOne(sb, goal,
                    o ? o->ovrSpringStiffness : cfg.springStiffness,
                    o ? o->ovrSpringDamping   : cfg.springDamping,
                    o ? o->ovrSpringMass      : cfg.springMass,
                    o ? o->ovrSpringGravity   : cfg.springGravity,
                    o ? o->ovrSpringMaxDisp   : cfg.springMaxDisp);
    }

    // Pass 2: children of virtual roots; goal = the child's animated position +
    // the root's lag.
    for (auto& sb : data.springBones)
    {
        if (sb.parentSpringIdx < 0) continue;
        const SpringBone& parent = data.springBones[sb.parentSpringIdx];

        // Facing-correct goal: the child's TRUE animated FK position + the virtual
        // root's inertial lag (currentWorldPos - animMidpoint). It tracks the body at
        // any facing and settles to rest (lag -> 0). Do not use a bind-space child
        // offset here: frozen at the bind pose's facing, it points to the WRONG side
        // after a 180 deg turn and leaves a permanent rotation + skin stretch.
        XMFLOAT3 childRest = ComputeBindWorldPos(sb.boneIndex, skel, poses);
        XMFLOAT3 goal = {
            childRest.x + (parent.currentWorldPos.x - parent.animMidpoint.x),
            childRest.y + (parent.currentWorldPos.y - parent.animMidpoint.y),
            childRest.z + (parent.currentWorldPos.z - parent.animMidpoint.z)
        };
        // Frozen = held AT its animated position (not the parent's lagged goal: a
        // frozen child of a soloed virtual root gets no swing).
        if (sb.frozen) { sb.currentWorldPos = childRest; sb.velocity = { 0.f, 0.f, 0.f }; continue; }

        // Children read their OWN override fields: the root and child globals differ
        // by up to 3x, and one shared ovrSpring* set would collapse the children onto
        // the root values once a group enables its override.
        const ChainGroupDef* o = grpOvr(sb.groupIdx);
        simulateOne(sb, goal,
                    o ? o->ovrSpringChildStiffness : cfg.springChildStiffness,
                    o ? o->ovrSpringChildDamping   : cfg.springChildDamping,
                    o ? o->ovrSpringChildMass      : cfg.springChildMass,
                    o ? o->ovrSpringChildGravity   : cfg.springChildGravity,
                    o ? o->ovrSpringChildMaxDisp   : cfg.springChildMaxDisp);
    }

    // Pass 3: independent spring bones (no virtual parent).
    for (auto& sb : data.springBones)
    {
        if (sb.isVirtual || sb.parentSpringIdx >= 0) continue;

        XMFLOAT3 goalWorld = ComputeBindWorldPos(sb.boneIndex, skel, poses);
        if (sb.frozen) { sb.currentWorldPos = goalWorld; sb.velocity = { 0.f, 0.f, 0.f }; continue; }   // frozen by a preview solo: held at the goal
        const ChainGroupDef* o = grpOvr(sb.groupIdx);
        simulateOne(sb, goalWorld,
                    o ? o->ovrSpringStiffness : cfg.springStiffness,
                    o ? o->ovrSpringDamping   : cfg.springDamping,
                    o ? o->ovrSpringMass      : cfg.springMass,
                    o ? o->ovrSpringGravity   : cfg.springGravity,
                    o ? o->ovrSpringMaxDisp   : cfg.springMaxDisp);
    }
}

// ==== WriteSpringBonePoses: spring world position -> local rotation ====
// The simulated position's displacement from the animation goal becomes a
// local rotation delta in the parent bone's frame.
void ChainPhysicsSystem::WriteSpringBonePoses(
    EntityData& data,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    for (auto& sb : data.springBones)
    {
        if (sb.isVirtual) continue;

        uint32_t bi = sb.boneIndex;
        if (bi >= skel.boneCount) continue;
        int32_t parentIdx = skel.parentIndex[bi];
        if (parentIdx < 0) continue;

        XMMATRIX parentWorld = ComputeWorldTransform(
            static_cast<uint32_t>(parentIdx), poses, skel);
        XMMATRIX parentInv = XMMatrixInverse(nullptr, parentWorld);

        XMVECTOR bindLocal = XMVectorSet(
            sb.restLocalPos.x, sb.restLocalPos.y, sb.restLocalPos.z, 0.f);
        float bindLen = XMVectorGetX(XMVector3Length(bindLocal));
        if (bindLen < 1e-6f) continue;
        XMVECTOR bindDir = XMVectorScale(bindLocal, 1.f / bindLen);

        XMFLOAT3 goalWorld = ComputeBindWorldPos(bi, skel, poses);
        XMVECTOR displacement = XMVectorSubtract(
            XMLoadFloat3(&sb.currentWorldPos),
            XMLoadFloat3(&goalWorld));

        XMVECTOR dispLocal = XMVector3TransformNormal(displacement, parentInv);

        // The displaced tip in parent space is (restLocalPos + dispLocal), both in
        // MODEL units. Add the UN-normalized bindLocal, NOT bindDir (unit length), or
        // the swing angle is exaggerated by the bone length L = |restLocalPos|
        // (atan(disp/1) instead of atan(disp/L)), which over-rotates short jiggle
        // bones and distorts under fast motion.
        XMVECTOR simLocal = XMVector3Normalize(
            XMVectorAdd(bindLocal, dispLocal));

        float dot = XMVectorGetX(XMVector3Dot(bindDir, simLocal));
        dot = std::min(std::max(dot, -1.f), 1.f);
        if (dot > 0.9999f) continue;

        XMVECTOR cross = XMVector3Cross(bindDir, simLocal);
        float crossLen = XMVectorGetX(XMVector3Length(cross));
        if (crossLen < 1e-8f) continue;

        float angle = std::acosf(dot);
        XMVECTOR axis  = XMVector3Normalize(cross);
        XMVECTOR swing = XMQuaternionRotationAxis(axis, angle);  // parent-frame jiggle delta

        // COMPOSE the jiggle onto the bone's rest / animation rotation; do NOT replace
        // it. poses[bi].rot holds the clean base this frame (AnimationSystem, or
        // RunPass's resetRot). The swing is a PARENT-frame rotation (bindDir / simLocal
        // live in parent space) applied AFTER R_rest: row-vector S*R*T =>
        // R(R_rest)*R(swing) => Multiply(R_rest, swing) (DirectXMath applies the first
        // argument first). Overwriting would drop any non-identity chest bind rotation
        // (~45-90 deg) and leave a displaced chest with a flipped local Z and skin
        // stretch. swing == identity at rest keeps R_rest.
        XMVECTOR rest     = XMLoadFloat4(&poses[bi].rot);
        XMVECTOR finalRot = XMQuaternionMultiply(rest, swing);
        XMStoreFloat4(&poses[bi].rot, XMQuaternionNormalize(finalRot));
    }
}

// ==== Capsule collision: ClosestPtSegmentSegment / SolveCapsuleCollision ====
// Closest points between segments [p1,q1] and [p2,q2]: returns the squared
// distance and outputs the parameters s (on seg 1) / t (on seg 2) and the
// closest points c1 / c2 (Ericson, Real-Time Collision Detection 5.1.9).
static float ClosestPtSegmentSegment(
    const XMFLOAT3& p1, const XMFLOAT3& q1,
    const XMFLOAT3& p2, const XMFLOAT3& q2,
    float& s, float& t, XMFLOAT3& c1, XMFLOAT3& c2)
{
    auto sub = [](const XMFLOAT3& a, const XMFLOAT3& b){ return XMFLOAT3{ a.x-b.x, a.y-b.y, a.z-b.z }; };
    auto dot = [](const XMFLOAT3& a, const XMFLOAT3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; };
    const XMFLOAT3 d1 = sub(q1, p1); // seg1 direction
    const XMFLOAT3 d2 = sub(q2, p2); // seg2 direction
    const XMFLOAT3 r  = sub(p1, p2);
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    constexpr float EPS = 1e-8f;
    auto clamp01 = [](float v){ return std::min(std::max(v, 0.f), 1.f); };

    if (a <= EPS && e <= EPS) { s = t = 0.f; }
    else if (a <= EPS)        { s = 0.f; t = clamp01(f / e); }
    else
    {
        const float c = dot(d1, r);
        if (e <= EPS) { t = 0.f; s = clamp01(-c / a); }
        else
        {
            const float b     = dot(d1, d2);
            const float denom = a*e - b*b;
            s = (denom > EPS) ? clamp01((b*f - c*e) / denom) : 0.f;
            t = (b*s + f) / e;
            if      (t < 0.f) { t = 0.f; s = clamp01(-c / a); }
            else if (t > 1.f) { t = 1.f; s = clamp01((b - c) / a); }
        }
    }
    c1 = XMFLOAT3{ p1.x + d1.x*s, p1.y + d1.y*s, p1.z + d1.z*s };
    c2 = XMFLOAT3{ p2.x + d2.x*t, p2.y + d2.y*t, p2.z + d2.z*t };
    const XMFLOAT3 d = sub(c1, c2);
    return dot(d, d);
}

// Pushes cloth out of capsule colliders:
// A) NODE: each particle (bone joint) vs each capsule, with a swept march for
//    fast motion so a particle cannot tunnel a thin capsule in one step.
// B) LINK: each constraint segment vs each capsule (segment-segment closest
//    point). Vertical links (along a chain) stop a straight bone chord from
//    cutting through a convex body part; horizontal / shear links (between two
//    adjacent skirt chains) stop the panel from clipping when a limb enters
//    the gap while every particle is outside every capsule.
// C) CLEANUP: two static node sweeps, only when A or B moved something.
//
// The effective radius is (cap.radius + cloth thickness) x skin, so the
// SKINNED surface, not just the bone centerline, clears the body. The
// thickness is Particle::radius, baked each frame in Simulate from
// ChainPhysicsComponent::collisionPadding.
void ChainPhysicsSystem::SolveCapsuleCollision(
    EntityData& data, const Island& isl,
    const WorldCapsule* capsules, int capsuleCount,
    const WorldCapsule* capsulesPrev,
    bool sweepParticles, bool applyFriction,
    float skin)
{
    // Capsule collision gets its own zone so Chain.Solve's SELF time
    // (tracy-csvexport -e) is the PBD constraint projection alone: the
    // per-iteration collision calls nested inside Chain.Solve land here. That
    // split says whether SIMD-ing the constraint solver would be worth an
    // AoS -> SoA restructure of Particle.
    ZoneScopedN("Chain.Collide");

    struct NodeQueryOptions { bool baseline = false; bool verify = false; };
    static const NodeQueryOptions s_nodeOptions = [] {
        NodeQueryOptions options;
#if WITH_HARNESS
        options.baseline = Sys::CommandLine::Get().Has("-chainnodebaseline");
        options.verify = Sys::CommandLine::Get().Has("-verifychainnode");
#endif
        LOG_INFO("[CHAIN_NODE] query=%s verify=%s",
                 options.baseline ? "legacy" : "box-filtered", options.verify ? "on" : "off");
        return options;
    }();

    // Entry-side snapshot for the tunneling test. velResponse rewrites
    // prevPosition on every contact (including Pass B feedback), so the side
    // reference must be frozen once per pass or later contacts corrupt it.
    // Scratch buffers were pre-sized in InitEntity; only this island's range is
    // touched, so concurrent islands never alias.
    for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
        data.prevPosSnapshotScratch[i] = data.particles[i].prevPosition;

    // Contact-response tuning:
    //  skin       - ChainPhysicsComponent::contactSkin (default 1.015) inflates the
    //               contact radius so cloth rests just OFF the surface, which stops
    //               on-boundary in / out toggling. The caller clamps it to >= 1;
    //               below 1 the cloth sinks into the body.
    //  p.friction - ChainPhysicsComponent::contactFriction or a group's
    //               ovrContactFriction, baked per particle in Simulate (this
    //               function is island-scoped and sees neither the config nor a
    //               particle's group). The fraction of tangential velocity removed
    //               on contact: 0 = frictionless slide, 1 = full grip. Applied ONLY
    //               on the swept pass (see velResponse).

    // ---- Island / capsule broad phase ----
    // This function runs nSub * (1 + cfg.iterations) + 1 times per island per
    // frame (~65 calls at the adaptive substep ceiling), and every call is
    // O(particles * capsules) for the node passes plus O(constraints * capsules)
    // segment-segment tests for the link proxies. On a humanoid most body capsules
    // are nowhere near any one cloth island, so reject them ONCE here and let all
    // three passes iterate only the survivors.
    //
    // Conservative by construction: the island box spans every position the
    // passes can sample (the substep-start prevPosition, the integrated end
    // position and the swept march between them), each capsule box covers both
    // sampled poses inflated by its full contact radius, and the island box gets
    // a 2x contact-radius margin so intra-call displacement (Pass A's push, Pass
    // B's proxy feedback, the two cleanup sweeps) cannot carry a particle into a
    // rejected capsule. A capsule is dropped only when it provably cannot produce
    // a contact this call.
    int activeCaps[CapsuleColliderComponent::MAX_CAPSULES];
    int activeCount = 0;
    // The per-capsule boxes this broad phase already builds are KEPT, not
    // discarded: Pass B re-tests every constraint against every survivor at
    // segment-segment cost, and these boxes are a valid conservative reject for
    // that test (see the note at Pass B's use). Inflated by maxParticleR, which
    // is >= any single constraint's own thickness, so the box is never tighter
    // than the pair's real contact radius.
    struct CapBox { float lo[3]; float hi[3]; };
    CapBox activeBox[CapsuleColliderComponent::MAX_CAPSULES];
    CapBox nodeBox[CapsuleColliderComponent::MAX_CAPSULES];
    {
        float loX = 1e30f, loY = 1e30f, loZ = 1e30f;
        float hiX = -1e30f, hiY = -1e30f, hiZ = -1e30f;
        float maxParticleR = 0.f;
        for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
        {
            const Particle& p = data.particles[i];
            loX = std::min(loX, std::min(p.position.x, p.prevPosition.x));
            loY = std::min(loY, std::min(p.position.y, p.prevPosition.y));
            loZ = std::min(loZ, std::min(p.position.z, p.prevPosition.z));
            hiX = std::max(hiX, std::max(p.position.x, p.prevPosition.x));
            hiY = std::max(hiY, std::max(p.position.y, p.prevPosition.y));
            hiZ = std::max(hiZ, std::max(p.position.z, p.prevPosition.z));
            if (p.radius > maxParticleR) maxParticleR = p.radius;
        }
        if (hiX < loX) return;   // empty island: nothing to collide

        float maxContactR = 0.f;
        for (int c = 0; c < capsuleCount; ++c)
            maxContactR = std::max(maxContactR, (capsules[c].radius + maxParticleR) * skin);

        const float margin = 2.f * maxContactR;
        loX -= margin; loY -= margin; loZ -= margin;
        hiX += margin; hiY += margin; hiZ += margin;

        for (int c = 0; c < capsuleCount; ++c)
        {
            const WorldCapsule& cc = capsules[c];
            const float r = (cc.radius + maxParticleR) * skin;
            float cLoX = std::min(cc.a.x, cc.b.x) - r, cHiX = std::max(cc.a.x, cc.b.x) + r;
            float cLoY = std::min(cc.a.y, cc.b.y) - r, cHiY = std::max(cc.a.y, cc.b.y) + r;
            float cLoZ = std::min(cc.a.z, cc.b.z) - r, cHiZ = std::max(cc.a.z, cc.b.z) + r;
            if (capsulesPrev)   // the tunneling side test also reads the substep-start pose
            {
                const WorldCapsule& cp = capsulesPrev[c];
                cLoX = std::min(cLoX, std::min(cp.a.x, cp.b.x) - r);
                cHiX = std::max(cHiX, std::max(cp.a.x, cp.b.x) + r);
                cLoY = std::min(cLoY, std::min(cp.a.y, cp.b.y) - r);
                cHiY = std::max(cHiY, std::max(cp.a.y, cp.b.y) + r);
                cLoZ = std::min(cLoZ, std::min(cp.a.z, cp.b.z) - r);
                cHiZ = std::max(cHiZ, std::max(cp.a.z, cp.b.z) + r);
            }
            if (cHiX < loX || cLoX > hiX) continue;
            if (cHiY < loY || cLoY > hiY) continue;
            if (cHiZ < loZ || cLoZ > hiZ) continue;
            activeBox[activeCount] = { { cLoX, cLoY, cLoZ }, { cHiX, cHiY, cHiZ } };
            if (!s_nodeOptions.baseline || s_nodeOptions.verify)
            {
                nodeBox[activeCount] = activeBox[activeCount];
                // Round outward with slack for the float distance / projection
                // arithmetic. Only node rejection uses these looser boxes; Pass B's
                // link filter keeps the exact activeBox.
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float magnitude = std::max(1.f, std::max(
                        std::abs(nodeBox[activeCount].lo[axis]),
                        std::abs(nodeBox[activeCount].hi[axis])));
                    const float slack = 8.f * std::numeric_limits<float>::epsilon() * magnitude;
                    nodeBox[activeCount].lo[axis] -= slack;
                    nodeBox[activeCount].hi[axis] += slack;
                }
            }
            activeCaps[activeCount++] = c;
        }
        if (activeCount == 0) return;   // no capsule can reach this island
    }

    // Verlet velocity response along outward normal n (unit): cancel the INWARD
    // normal component (no ram -> no bounce -> no jitter), damp tangential (friction),
    // preserve separating motion (vn > 0) so cloth can still lift off freely.
    auto velResponse = [&](Particle& p, float nx, float ny, float nz, bool dampFriction)
    {
        const float vx = p.position.x - p.prevPosition.x;
        const float vy = p.position.y - p.prevPosition.y;
        const float vz = p.position.z - p.prevPosition.z;
        const float vn = vx*nx + vy*ny + vz*nz;
        const float tx = vx - vn*nx, ty = vy - vn*ny, tz = vz - vn*nz;
        const float vnKeep = (vn > 0.f) ? vn : 0.f;
        // Friction is applied only on the one swept contact pass. Static
        // projections run after every distance-constraint iteration; damping
        // tangential motion on all of them would multiply friction by the PBD
        // iteration count and make cloth stick/buzz against the body.
        const float tangentScale = dampFriction ? (1.f - p.friction) : 1.f;
        p.prevPosition.x = p.position.x - (tx * tangentScale + vnKeep * nx);
        p.prevPosition.y = p.position.y - (ty * tangentScale + vnKeep * ny);
        p.prevPosition.z = p.position.z - (tz * tangentScale + vnKeep * nz);
    };

    // Resolve one particle at its CURRENT position against a single capsule.
    // Returns true if a contact was pushed out.
    //
    // prevSnap = the particle's position at pass entry (see snapshot above).
    // capPrev  = the same capsule at the start of this substep (nullable): with
    // it, the entry side survives a fast collider sweeping past a slow particle
    // (relative to the CURRENT pose, both prev and current offsets would already
    // sit on the far side and the crossing would be invisible).
    auto resolveStaticExact = [&](Particle& p, const XMFLOAT3& prevSnap,
                             const WorldCapsule& cap, const WorldCapsule* capPrev,
                             bool dampFriction) -> bool
    {
        const float r = (cap.radius + p.radius) * skin;
        const float abx = cap.b.x - cap.a.x, aby = cap.b.y - cap.a.y, abz = cap.b.z - cap.a.z;
        const float apx = p.position.x - cap.a.x, apy = p.position.y - cap.a.y, apz = p.position.z - cap.a.z;
        const float abDot = abx*abx + aby*aby + abz*abz;
        const float t = (abDot > 1e-8f)
            ? std::max(0.f, std::min(1.f, (apx*abx + apy*aby + apz*abz) / abDot)) : 0.f;

        float dx = p.position.x - (cap.a.x + t * abx);
        float dy = p.position.y - (cap.a.y + t * aby);
        float dz = p.position.z - (cap.a.z + t * abz);
        // Reject on the SQUARED distance: this runs nSub * (1 + iterations) + 1
        // times per island per frame (~65) for every particle x surviving capsule,
        // and ~99% of the calls take this early out, so the sqrt runs only on a
        // contact. The two forms differ only in a <= 1-ulp band at d2 == r*r, which
        // is inert: contactSkin already keeps cloth OFF the surface and the
        // projection re-runs ~65x per frame.
        const float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 >= r * r) return false; // outside: no contact
        float dist = std::sqrtf(d2);

        // Entry-side offset: prevSnap relative to the substep-start capsule pose
        // (or this capsule when no start pose was provided). Shared by the
        // tunneling test and the on-axis degenerate fallback below.
        const WorldCapsule& pc = capPrev ? *capPrev : cap;
        const float pbx = pc.b.x - pc.a.x, pby = pc.b.y - pc.a.y, pbz = pc.b.z - pc.a.z;
        const float pabDot = pbx*pbx + pby*pby + pbz*pbz;
        const float pApx = prevSnap.x - pc.a.x, pApy = prevSnap.y - pc.a.y, pApz = prevSnap.z - pc.a.z;
        const float pT = (pabDot > 1e-8f)
            ? std::max(0.f, std::min(1.f, (pApx*pbx + pApy*pby + pApz*pbz) / pabDot)) : 0.f;
        const float pox = prevSnap.x - (pc.a.x + pT * pbx);
        const float poy = prevSnap.y - (pc.a.y + pT * pby);
        const float poz = prevSnap.z - (pc.a.z + pT * pbz);
        const float poLen = std::sqrtf(pox*pox + poy*poy + poz*poz);

        // Outward contact normal. Degenerate case (particle exactly on the axis):
        // eject along a stable axis-perpendicular direction so it is never trapped.
        float nx, ny, nz;
        if (dist > 1e-5f)
        {
            const float inv = 1.f / dist; nx = dx*inv; ny = dy*inv; nz = dz*inv;

            if (poLen > 1e-5f)
            {
                const float ipl = 1.f / poLen;
                const float npx = pox*ipl, npy = poy*ipl, npz = poz*ipl;
                const float side = dx*npx + dy*npy + dz*npz;
                if (side < 0.f)
                {
                    // The particle sits on the FAR side of where it entered the
                    // pass: the nearest-surface pushout below would eject it through
                    // the body (the low-substep pop-through). Tell true tunneling
                    // from a legitimate slide AROUND the capsule by sampling the
                    // chord prevSnap -> position where it crosses the entry side
                    // (u*, offset perpendicular to the entry normal). Tunneling
                    // crosses INSIDE the body; a wraparound arc crosses outside.
                    const float f0 = poLen;          // dot(prevOffset, nPrev)
                    const float f1 = side;           // dot(curOffset,  nPrev) < 0
                    const float u  = f0 / (f0 - f1); // linear zero crossing
                    const float Xx = prevSnap.x + (p.position.x - prevSnap.x) * u;
                    const float Xy = prevSnap.y + (p.position.y - prevSnap.y) * u;
                    const float Xz = prevSnap.z + (p.position.z - prevSnap.z) * u;
                    const float xApx = Xx - cap.a.x, xApy = Xy - cap.a.y, xApz = Xz - cap.a.z;
                    const float xT = (abDot > 1e-8f)
                        ? std::max(0.f, std::min(1.f, (xApx*abx + xApy*aby + xApz*abz) / abDot)) : 0.f;
                    const float xdx = Xx - (cap.a.x + xT * abx);
                    const float xdy = Xy - (cap.a.y + xT * aby);
                    const float xdz = Xz - (cap.a.z + xT * abz);
                    if (std::sqrtf(xdx*xdx + xdy*xdy + xdz*xdz) < r)
                    {
                        // True tunneling: eject on the ENTRY side. Penetration is
                        // measured along the entry normal (side < 0 => pen > r).
                        const float pen = r - side;
                        p.position.x += npx * pen;
                        p.position.y += npy * pen;
                        p.position.z += npz * pen;
                        // A cross-body teleport is not physical motion: kill the
                        // Verlet velocity instead of running velResponse, which
                        // would read the ejection as separating speed (~pen per
                        // step) and launch the particle off the surface.
                        p.prevPosition = p.position;
                        return true;
                    }
                    // else: a wraparound; the nearest-side pushout below is correct.
                }
            }
        }
        else
        {
            // KawaiiPhysics falls back to a stable collider side axis. WorldCapsule
            // stores no roll, so prefer the entry side for continuity; only a
            // particle that also ENTERED on-axis uses a derived axis.
            if (poLen > 1e-5f)
            {
                nx = pox / poLen; ny = poy / poLen; nz = poz / poLen;
            }
            else
            {
                float px = -abz, pz = abx;
                const float pl = std::sqrtf(px*px + pz*pz);
                if (pl > 1e-5f) { nx = px / pl; ny = 0.f; nz = pz / pl; }
                else            { nx = 1.f;     ny = 0.f; nz = 0.f;      }
            }
            dist = 0.f;
        }

        const float pen = r - dist;
        p.position.x += nx * pen;
        p.position.y += ny * pen;
        p.position.z += nz * pen;
        velResponse(p, nx, ny, nz, dampFriction);
        return true;
    };

    auto resolveStatic = [&](Particle& p, const XMFLOAT3& prevSnap,
                             int ai, bool dampFriction) -> bool
    {
        const int c = activeCaps[ai];
        const WorldCapsule* prev = capsulesPrev ? &capsulesPrev[c] : nullptr;
        if (!s_nodeOptions.baseline)
        {
            // The current particle must lie inside the capsule AABB inflated
            // by its contact radius to touch it. nodeBox (activeBox plus float
            // slack) covers both capsule poses and the island's MAX particle
            // radius, so it is conservative for every node, including swept march
            // samples and particles displaced by earlier capsules / cleanup passes.
            const CapBox& box = nodeBox[ai];
            if (p.position.x < box.lo[0] || p.position.x > box.hi[0] ||
                p.position.y < box.lo[1] || p.position.y > box.hi[1] ||
                p.position.z < box.lo[2] || p.position.z > box.hi[2])
            {
                if (s_nodeOptions.verify)
                {
                    Particle reference = p;
                    const bool contact = resolveStaticExact(reference, prevSnap,
                        capsules[c], prev, dampFriction);
                    ENGINE_CHECK(!contact &&
                        std::memcmp(&reference.position, &p.position, sizeof(XMFLOAT3)) == 0 &&
                        std::memcmp(&reference.prevPosition, &p.prevPosition, sizeof(XMFLOAT3)) == 0,
                        "Chain node box rejected an effective capsule contact");
                    static std::atomic<uint64_t> s_verified{0};
                    const uint64_t count = s_verified.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (count % 1000000 == 0)
                        LOG_INFO("[CHAIN_NODE_AUDIT] verifiedRejects=%llu",
                                 static_cast<unsigned long long>(count));
                }
                return false;
            }
        }
        return resolveStaticExact(p, prevSnap, capsules[c], prev, dampFriction);
    };

    // The three passes are zoned separately: Pass B is O(links x capsules)
    // segment-segment while A and C are O(particles x capsules) point tests, so
    // which one to optimise depends on the scene's link:particle ratio. Zone per
    // PASS only, never per particle: a per-iteration zone would cost more than
    // the work it measures. activeCaps is plotted to judge the broad phase above
    // (a high survivor count means a per-strand refinement would pay).
#ifdef TRACY_ENABLE
    TracyPlot("Chain.activeCaps", static_cast<int64_t>(activeCount));
#endif

    // Did anything actually move? Drives the cleanup-sweep skip below.
    bool anyNodeContact = false;   // Pass A resolved at least one penetration
    bool anyProxyPush   = false;   // Pass B displaced at least one particle

    // ---- Pass A: node (per-particle) collision, with a swept march ----
    {
    ZoneScopedN("Chain.Collide.Particle");
    for (uint32_t pi = isl.particleBegin; pi < isl.particleEnd; ++pi)
    {
        Particle& p = data.particles[pi];
        const XMFLOAT3& prevSnap = data.prevPosSnapshotScratch[pi];
        if (p.invMass <= 0.f) continue; // skip pinned roots

        if (!sweepParticles)
        {
            for (int ai = 0; ai < activeCount; ++ai)
            {
                resolveStatic(p, prevSnap, ai, false);
            }
            continue;
        }

        // Smallest effective radius across capsules sets the march step so a moving
        // particle samples the path finely enough that it cannot skip a thin capsule.
        // Only the broad-phase survivors count: a capsule that cannot be hit must
        // not force a finer march.
        float rmin = 1e30f;
        for (int ai = 0; ai < activeCount; ++ai)
        {
            const float rr = (capsules[activeCaps[ai]].radius + p.radius) * skin;
            if (rr < rmin) rmin = rr;
        }

        const XMFLOAT3 A = p.prevPosition;   // substep start
        const XMFLOAT3 B = p.position;       // integrated + constrained end
        const float mlen = std::sqrtf((B.x-A.x)*(B.x-A.x) + (B.y-A.y)*(B.y-A.y) + (B.z-A.z)*(B.z-A.z));

        // Only fast particles (moving farther than the thinnest capsule) pay for the
        // march; slow motion can't cross a capsule in one step, so K=1 == static test.
        int K = 1;
        if (rmin < 1e30f && mlen > rmin)
            K = std::min(8, static_cast<int>(std::ceil(mlen / (0.5f * rmin))));

        if (K <= 1)
        {
            for (int ai = 0; ai < activeCount; ++ai)
            {
                if (resolveStatic(p, prevSnap, ai, applyFriction))
                    anyNodeContact = true;
            }
        }
        else
        {
            // Walk A->B; clamp at the FIRST penetrating sample so the particle stops
            // on the ENTRY side instead of being ejected out the far side (thin-capsule
            // deep-penetration inversion). No penetration => last sample is B (static).
            bool hit = false;
            anyNodeContact = true;   // conservative: the swept branch may move p
            for (int i = 1; i <= K && !hit; ++i)
            {
                const float u = static_cast<float>(i) / static_cast<float>(K);
                p.position = { A.x + (B.x-A.x)*u, A.y + (B.y-A.y)*u, A.z + (B.z-A.z)*u };
                for (int ai = 0; ai < activeCount; ++ai)
                {
                    if (resolveStatic(p, prevSnap, ai, applyFriction))
                        hit = true;
                }
            }
        }
    }
    }   // Chain.Collide.Particle

    // ---- Pass B: KawaiiPhysics-style link collision proxies ----
    // A cloth particle is shared by vertical, horizontal and shear links, so
    // projecting every penetrating link at full strength would give a joint
    // touched by N links N independent corrections in one pass. Like
    // KawaiiPhysics' subdivision / bridge dummies, but with no persistent dummy
    // particles: each exact segment / capsule closest point is a virtual proxy
    // whose push is accumulated, then applied once per endpoint (weight-averaged).
    {
    ZoneScopedN("Chain.Collide.LinkProxy");
    for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
    {
        data.collisionPushScratch[i]   = { 0.f, 0.f, 0.f };
        data.collisionWeightScratch[i] = 0.f;
    }

    for (int g = 0; g < data.groupCount; ++g)
    {
        for (uint32_t ci = isl.constraintBegin[g]; ci < isl.constraintEnd[g]; ++ci)
        {
            const Constraint& con = data.constraintGroups[g][ci];
            Particle& pA = data.particles[con.particleA];
            Particle& pB = data.particles[con.particleB];
            if (pA.invMass <= 0.f && pB.invMass <= 0.f) continue; // both pinned

            const float thick = std::max(pA.radius, pB.radius);

            // Link AABB, built once per constraint and tested against each
            // survivor's box below. A contact needs a point of segment AB within
            // r of the capsule axis, and such a point lies in BOTH this box and
            // the capsule's inflated box, so disjoint boxes prove there is no
            // contact (six compares instead of a segment-segment solve). A
            // pre-filter only: the d2 >= r*r test below stays the authority.
            const float lLoX = std::min(pA.position.x, pB.position.x);
            const float lHiX = std::max(pA.position.x, pB.position.x);
            const float lLoY = std::min(pA.position.y, pB.position.y);
            const float lHiY = std::max(pA.position.y, pB.position.y);
            const float lLoZ = std::min(pA.position.z, pB.position.z);
            const float lHiZ = std::max(pA.position.z, pB.position.z);

            for (int ai = 0; ai < activeCount; ++ai)
            {
                const CapBox& cb = activeBox[ai];
                if (lHiX < cb.lo[0] || lLoX > cb.hi[0]) continue;
                if (lHiY < cb.lo[1] || lLoY > cb.hi[1]) continue;
                if (lHiZ < cb.lo[2] || lLoZ > cb.hi[2]) continue;

                const WorldCapsule& cap = capsules[activeCaps[ai]];
                const float r = (cap.radius + thick) * skin;

                float s, t; XMFLOAT3 c1, c2;
                // Same squared-distance reject as resolveStatic. The hottest
                // loop of the collide pass: vertical + horizontal + shear links
                // make it ~3x the particle count against every surviving
                // capsule, at segment-segment cost.
                const float d2 = ClosestPtSegmentSegment(pA.position, pB.position, cap.a, cap.b, s, t, c1, c2);
                if (d2 >= r * r) continue;
                const float dist = std::sqrtf(d2);

                // Outward normal from the capsule-axis point c2 to the cloth point c1.
                float nx, ny, nz;
                if (dist > 1e-5f) { const float inv = 1.f / dist; nx = (c1.x-c2.x)*inv; ny = (c1.y-c2.y)*inv; nz = (c1.z-c2.z)*inv; }
                else
                {
                    const float abx = cap.b.x - cap.a.x, abz = cap.b.z - cap.a.z;
                    float px = -abz, pz = abx;
                    const float pl = std::sqrtf(px*px + pz*pz);
                    if (pl > 1e-5f) { nx = px / pl; ny = 0.f; nz = pz / pl; }
                    else            { nx = 1.f;     ny = 0.f; nz = 0.f;      }
                }

                const float pen = r - dist;
                const float wA = pA.invMass, wB = pB.invMass;
                float aWeight = (1.f - s) * wA;
                float bWeight = s * wB;
                // A proxy close to a pinned root still needs to move the free end;
                // hand the full feedback to the only movable endpoint.
                if (wA <= 0.f && wB > 0.f) bWeight = 1.f;
                if (wB <= 0.f && wA > 0.f) aWeight = 1.f;

                if (aWeight > 0.f)
                {
                    XMFLOAT3& push = data.collisionPushScratch[con.particleA];
                    push.x += nx * pen * aWeight;
                    push.y += ny * pen * aWeight;
                    push.z += nz * pen * aWeight;
                    data.collisionWeightScratch[con.particleA] += aWeight;
                }
                if (bWeight > 0.f)
                {
                    XMFLOAT3& push = data.collisionPushScratch[con.particleB];
                    push.x += nx * pen * bWeight;
                    push.y += ny * pen * bWeight;
                    push.z += nz * pen * bWeight;
                    data.collisionWeightScratch[con.particleB] += bWeight;
                }
            }
        }
    }

    for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
    {
        Particle& p = data.particles[i];
        const float weight = data.collisionWeightScratch[i];
        if (p.invMass <= 0.f || weight <= 0.f) continue;

        const float invDivisor = 1.f / std::max(1.f, weight);
        const XMFLOAT3 delta = {
            data.collisionPushScratch[i].x * invDivisor,
            data.collisionPushScratch[i].y * invDivisor,
            data.collisionPushScratch[i].z * invDivisor
        };
        p.position.x += delta.x;
        p.position.y += delta.y;
        p.position.z += delta.z;

        const float len = std::sqrtf(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
        if (len > 1e-6f)
        {
            anyProxyPush = true;
            velResponse(p, delta.x / len, delta.y / len, delta.z / len, false);
        }
    }
    }   // Chain.Collide.LinkProxy

    // ---- Pass C: two static cleanup sweeps ----
    // They (a) catch an endpoint that Pass B's feedback pushed into a
    // neighbouring capsule after Pass A visited it, and (b) converge overlapping
    // body capsules at joints, which sequential per-capsule projection leaves
    // slightly penetrating. No friction here, so the swept pass stays the single
    // friction application per substep. Side tests reuse the pass-entry snapshot
    // (Pass B's velResponse has already rewritten prevPosition).
    //
    // Skipped when nothing moved: with no Pass A contact and no Pass B push,
    // every particle sits where Pass A already tested it against every surviving
    // capsule, so the sweeps are a provable no-op (a bit-exact skip). Worth
    // gating: they cost twice Pass A's node loop.
    if (anyNodeContact || anyProxyPush)
    {
    ZoneScopedN("Chain.Collide.Cleanup");
    for (int pass = 0; pass < 2; ++pass)
    {
        for (uint32_t pi = isl.particleBegin; pi < isl.particleEnd; ++pi)
        {
            Particle& p = data.particles[pi];
            if (p.invMass <= 0.f) continue;
            for (int ai = 0; ai < activeCount; ++ai)
            {
                resolveStatic(p, data.prevPosSnapshotScratch[pi], ai, false);
            }
        }
    }
    }   // Chain.Collide.Cleanup
}

// ==== PrepareSubsteps: per-entity pre-pass of RunPass's preparation ====
// The substep count and the Verlet displacement rescale are ENTITY-wide state
// (velocityStepSeconds, all particles), so they are resolved before the
// entity's islands fan out to concurrent jobs.
int ChainPhysicsSystem::PrepareSubsteps(
    EntityData& data, const ChainPhysicsComponent& cfg,
    const WorldCapsule* capsules, int capsuleCount, float dt, int subCap)
{
    const bool hasPrevCaps = capsules && capsuleCount > 0 &&
        data.prevCapsules.size() == static_cast<size_t>(capsuleCount);

    int nSub = std::max(1, std::min(cfg.substeps, 8));
    if (hasPrevCaps)
    {
        // The authored value is a minimum. With too few substeps a moving capsule
        // jumps from last frame's pose to this frame's, so a thin / fast limb could
        // cross the whole cloth without ever overlapping it: raise the count until
        // each sampled collider move is at most half the thinnest capsule radius
        // (capped at 8).
        float minRadius = 1e30f;
        float maxMotion = 0.f;
        for (int c = 0; c < capsuleCount; ++c)
        {
            const WorldCapsule& pc = data.prevCapsules[c];
            const WorldCapsule& cc = capsules[c];
            minRadius = std::min(minRadius,
                std::max(std::min(pc.radius, cc.radius), 1e-4f));

            const float dax = cc.a.x - pc.a.x, day = cc.a.y - pc.a.y, daz = cc.a.z - pc.a.z;
            const float dbx = cc.b.x - pc.b.x, dby = cc.b.y - pc.b.y, dbz = cc.b.z - pc.b.z;
            maxMotion = std::max(maxMotion, std::sqrtf(dax*dax + day*day + daz*daz));
            maxMotion = std::max(maxMotion, std::sqrtf(dbx*dbx + dby*dby + dbz*dbz));
        }
        if (minRadius < 1e30f && maxMotion > 0.5f * minRadius)
        {
            const int motionSubsteps = static_cast<int>(std::ceil(maxMotion / (0.5f * minRadius)));
            nSub = std::max(nSub, std::min(motionSubsteps, 8));
        }
    }

    // LOD cap, applied BEFORE the rescale below. Clamping the returned value at
    // the call site instead would leave data.velocityStepSeconds describing a step
    // count Simulate never ran, and next frame's stepRatio would kick the chain.
    nSub = std::max(1, std::min(nSub, subCap));

    const float stepSeconds = std::min(dt, 1.f / 30.f) / static_cast<float>(nSub);
    if (data.velocityStepSeconds > 0.f && stepSeconds != data.velocityStepSeconds)
    {
        // p.position - p.prevPosition is displacement per *previous* simulation
        // step. Preserve world velocity when FPS or automatic capsule CCD changes
        // the step duration; without this conversion, a 1->8 substep change
        // would replay a full-frame displacement eight times.
        const float stepRatio = stepSeconds / data.velocityStepSeconds;
        for (auto& p : data.particles)
        {
            if (p.invMass <= 0.f) continue;
            const float vx = (p.position.x - p.prevPosition.x) * stepRatio;
            const float vy = (p.position.y - p.prevPosition.y) * stepRatio;
            const float vz = (p.position.z - p.prevPosition.z) * stepRatio;
            p.prevPosition = { p.position.x - vx, p.position.y - vy, p.position.z - vz };
        }
    }

    data.velocityStepSeconds = stepSeconds;
    return nSub;
}

// ==== Simulate: one full simulation step for ONE island of an entity ====
// Runs as a TaskSystem job, possibly concurrent with the entity's other
// islands: everything indexed by particle id stays inside
// [isl.particleBegin, isl.particleEnd); constraint ranges are per island;
// poses[] writes are island-owned bones only (BuildIslands merged any island
// whose FK ancestors cross another island's writes). Shared reads
// (boneWorldMatCache, prevCapsules, cfg) are frame-constant during phase 2.
bool ChainPhysicsSystem::Simulate(
    EntityData& data, const Island& isl,
    const ChainPhysicsComponent& cfg,
    float dt, int nSub,
    int iterations, bool collideAllowed,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel,
    const WorldCapsule* capsules, int capsuleCount,
    float swayTime,
    const XMFLOAT3* gravityDown,
    const WorldKick* worldKick)
{
    ZoneScopedN("ChainPhysics.Simulate");

    // Clamp dt to 1/30 s to avoid explosion
    float cdt = std::min(dt, 1.f / 30.f);

    bool clampedDiverged = false;

    // boneWorldMatCache was composed in RunPass before Simulate runs; this reads
    // a bone's position from it (no chain walk).
    auto cachedBonePos = [&](uint32_t bi) -> XMFLOAT3 {
        if (bi >= skel.boneCount) return { 0,0,0 };
        const auto& m = data.boneWorldMatCache[bi];
        return { m._41, m._42, m._43 };
    };

    // ---- Chain physics (Verlet + PBD + capsule collision with substeps) ----
    if (isl.strandEnd > isl.strandBegin)
    {
        // Per-group collision opt-out (ChainGroupDef::collide). Capsule solving is
        // this system's dominant cost, and a chain held inside a tight rest-limit
        // cone can be authored to never reach the body. An island may hold several
        // groups (a cloth piece, a forked hair family, or a conflict merge), so skip
        // only when EVERY strand in it opts out (the conservative direction).
        // Resolved per frame, so toggling needs no Invalidate.
        bool islandCollide = false;
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd && !islandCollide; ++si)
        {
            const uint8_t gi = data.strands[si].groupIdx;
            islandCollide = !(gi < cfg.groups.size()) || cfg.groups[gi].collide;
        }

        // collideAllowed can only take collision AWAY: a group that authored
        // collide=false must never get it back from a LOD tier.
        const bool hasCapsules = capsules && capsuleCount > 0 && islandCollide
                                 && collideAllowed;
        const bool hasPrevCaps = hasCapsules &&
            data.prevCapsules.size() == static_cast<size_t>(capsuleCount);

        // nSub was resolved (and the velocity rescale applied) by PrepareSubsteps:
        // it is entity-wide state.
        const float subDt = cdt / static_cast<float>(nSub);

        // Per-group override (or nullptr -> the type-global params). Damping is
        // substep-adjusted per strand below (powf), since each chain may differ.
        auto grpOvr = [&](uint8_t gi) -> const ChainGroupDef* {
            return (gi < cfg.groups.size() && cfg.groups[gi].ovrEnabled) ? &cfg.groups[gi] : nullptr;
        };

        // Resolved PBD stiffness per authored group, split by CONSTRAINT BUCKET:
        // g[0..1] = vertical (along-chain), g[2..3] = horizontal (cloth hoop),
        // g[4..5] = shear (cloth diagonal). Three independent materials, not a
        // per-strand-type preset: the vertical builder runs for every strand, so a
        // Cloth piece's structural links read gsVert (the Chain-side global) and only
        // its hoop and diagonal links see the cloth values. 256 entries cover the
        // whole uint8_t groupIdx domain. Built once per entity per frame in its
        // preparation (EntityData::groupTables), so the solver loop below
        // (nSub * iterations visits per constraint) and every island task just index.
        const float* const gsVert        = data.groupTables.vert;
        const float* const gsRing        = data.groupTables.ring;
        const float* const gsShear       = data.groupTables.shear;
        const float* const horizDecayTab = data.groupTables.horizDecay;

        // Resolve each constraint's FINAL projection strength once per frame. Every
        // input is frame-constant (c.stiffness / segmentT / groupIdx baked at
        // InitEntity, the tables above and subDt per frame), while the solve visits
        // each constraint nSub * iterations times: inline substep compensation would
        // cost a powf per visit. Island-owned constraint ranges only, so concurrent
        // islands never alias.
        for (int g = 0; g < data.groupCount; ++g)
        {
            const bool isRing  = (g >= 2 && g < 4);  // cloth hoop links
            const bool isShear = (g >= 4);           // cloth diagonals
            for (uint32_t ci = isl.constraintBegin[g]; ci < isl.constraintEnd[g]; ++ci)
            {
                const Constraint& c = data.constraintGroups[g][ci];
                const float gs = isShear ? gsShear[c.groupIdx]
                               : (isRing ? gsRing[c.groupIdx] : gsVert[c.groupIdx]);

                float s = c.stiffness * gs;
                if (isRing || isShear)
                    s *= 1.f - c.segmentT * (1.f - horizDecayTab[c.groupIdx]);

                // The clamp MUST precede the compensation: powf's base is (1 - s),
                // so a stiffness above 1 gives a NEGATIVE base and a NaN that
                // poisons the whole island. It is also why the authored range
                // saturates: c.stiffness is a bucket constant (1.0 vertical / 0.8
                // ring / 0.5 shear), so the slider is inert above 1 / c.stiffness,
                // at any substep count.
                s = std::min(std::max(s, 0.f), 1.f);

                // Distribute the reference 60 Hz projection over elapsed time.
                // Coupled PBD/contact still depends on iteration convergence;
                // this is not an XPBD compliance or exact solver invariance.
                s = ChainPhysicsMath::Pull(s, subDt);

                data.constraintStiffScratch[g][ci] = s;
            }
        }

        // Resolve the local-stiffness blend factor per particle. It depends only on
        // the particle's position along its strand and on frame-constant config, but
        // the substep loop below needs it per particle PER SUBSTEP and it costs a
        // powf: hoisting saves ~2.7k powf calls per island per frame. Island-owned
        // particle range only, so concurrent islands never alias (same contract as
        // the other scratch buffers).
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
        {
            const Strand& strand = data.strands[si];
            const bool isSkirt = (strand.type == StrandType::Skirt);
            const ChainGroupDef* o = grpOvr(strand.groupIdx);
            const float ls = o ? o->ovrLocalStiffness
                               : (isSkirt ? cfg.skirtLocalStiffness : cfg.localStiffness);
            // Tip hold has a component-global (tipHold / skirtTipHold) besides the
            // group override, so a component without groups can use it too.
            const float tipHold = std::min(std::max(
                o ? o->ovrTipHold : (isSkirt ? cfg.skirtTipHold : cfg.tipHold), 0.f), 1.f);
            if (ls <= 0.f || strand.particleCount < 2) continue;

            const float chainLen = static_cast<float>(strand.particleCount);
            for (uint32_t i = 1; i < strand.particleCount; ++i)
            {
                const float t = static_cast<float>(i) / (chainLen - 1.f);
                float falloff = (1.f - t) * (1.f - t);
                if (falloff < tipHold) falloff = tipHold;
                const float frameBlend = std::min(std::max(ls * falloff, 0.f), 1.f);
                data.localBlendScratch[strand.particleOffset + i] =
                    ChainPhysicsMath::PosePull(frameBlend, subDt);
            }
        }

        // Bake the per-particle contact material here (not at init) so editing
        // collisionPadding / contactFriction, or a group's ovrCollisionPadding /
        // ovrContactFriction, is LIVE with no Invalidate.
        //  radius:   added to the capsule radius in SolveCapsuleCollision so the
        //            skinned surface offset from the bone clears the body, not just
        //            the bone centerline.
        //  friction: tangential velocity removed on contact. Lives on the particle
        //            because SolveCapsuleCollision is island-scoped and cannot see
        //            the config or a particle's owning group.
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
        {
            const Strand& strand = data.strands[si];
            const ChainGroupDef* o = grpOvr(strand.groupIdx);
            const float pad = std::max(o ? o->ovrCollisionPadding : cfg.collisionPadding, 0.f);
            const float fri = std::min(std::max(
                o ? o->ovrContactFriction : cfg.contactFriction, 0.f), 1.f);
            for (uint32_t i = 0; i < strand.particleCount; ++i)
            {
                Particle& p = data.particles[strand.particleOffset + i];
                p.radius   = pad;
                p.friction = fri;
            }
        }

        // Contact margin is entity-wide, so it rides as a call argument rather
        // than on the particle. Below 1 it would SHRINK the contact radius and
        // let cloth sink into the collider, so the clamp is not optional.
        const float contactSkin = std::max(cfg.contactSkin, 1.f);

        // Project a REST TARGET out of the colliders' contact fields. The local
        // pull blends particles toward the animated pose, and nothing stops that
        // pose from sitting inside a body capsule (crouch / leg lift vs a skirt): an
        // in-collider target re-injects the penetration every substep and the
        // contact pass ejects it again, a sustained buzz no damping can remove
        // because the energy source is positional. Moving the TARGET onto the
        // contact surface makes the pull convergent; no particle is touched here,
        // so no velocity is injected. Sequential per capsule like resolveStatic:
        // overlapping colliders keep last-writer semantics.
        auto declipTarget = [](XMFLOAT3& pt, float pRadius,
                               const WorldCapsule* caps, int nCaps, float sk)
        {
            for (int c = 0; c < nCaps; ++c)
            {
                const WorldCapsule& cap = caps[c];
                const float r = (cap.radius + pRadius) * sk;
                const float abx = cap.b.x - cap.a.x, aby = cap.b.y - cap.a.y,
                            abz = cap.b.z - cap.a.z;
                const float apx = pt.x - cap.a.x, apy = pt.y - cap.a.y,
                            apz = pt.z - cap.a.z;
                const float abDot = abx*abx + aby*aby + abz*abz;
                const float t = (abDot > 1e-8f)
                    ? std::max(0.f, std::min(1.f,
                          (apx*abx + apy*aby + apz*abz) / abDot)) : 0.f;
                const float cx = cap.a.x + t * abx;
                const float cy = cap.a.y + t * aby;
                const float cz = cap.a.z + t * abz;
                const float dx = pt.x - cx, dy = pt.y - cy, dz = pt.z - cz;
                const float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 >= r * r) continue;      // outside this capsule
                const float d = std::sqrtf(d2);
                if (d <= 1e-6f) continue;       // on the axis: no normal; the contact passes still guard it
                const float s = r / d;
                pt = { cx + dx * s, cy + dy * s, cz + dz * s };
            }
        };

        // Snapshot last frame's animation targets. KawaiiPhysics interpolates pose
        // targets across substeps; using this frame's final pose from the first
        // substep makes kinematic roots/local pull jump ahead of the interpolated
        // colliders and creates an artificial contact impulse.
        for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
            data.poseStartScratch[i] = data.particles[i].restPosition;

        // ---- Ambient idle sway ----
        // Resolve this frame's per-particle rest-target offset. Offsetting the
        // TARGET rather than adding a wind force makes the amplitude equal the
        // authored strength independently of stiffness / damping / frame rate, and
        // keeps the sway inside the rest-limit cone by construction (both are
        // measured against the same animated rest).
        //
        // The offset is scaled by the particle's position along its strand, and
        // each bone gets its own phase: the pull target is PARENT-RELATIVE, so a
        // uniform offset would cancel exactly and produce no motion at all.
        bool anyWind = false;
        {
            XMFLOAT3 windUnit = { 0.f, 0.f, 0.f };
            const float wl = std::sqrtf(cfg.windDir.x * cfg.windDir.x +
                                        cfg.windDir.y * cfg.windDir.y +
                                        cfg.windDir.z * cfg.windDir.z);
            if (wl > 1e-6f)
                windUnit = { cfg.windDir.x / wl, cfg.windDir.y / wl, cfg.windDir.z / wl };
            const bool dirValid = (wl > 1e-6f);
            const float turb = std::max(cfg.windTurbulence, 0.f);

            for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            {
                const Strand& strand = data.strands[si];
                const ChainGroupDef* o = grpOvr(strand.groupIdx);
                const float amp  = std::max(o ? o->ovrWindStrength : cfg.windStrength, 0.f);
                const float freq = std::max(o ? o->ovrWindFreq     : cfg.windFreq, 0.f);

                if (!dirValid || amp <= 0.f || strand.particleCount < 2)
                {
                    for (uint32_t i = 0; i < strand.particleCount; ++i)
                        data.windOffsetScratch[strand.particleOffset + i] = { 0.f, 0.f, 0.f };
                    continue;
                }
                anyWind = true;

                const float chainLen = static_cast<float>(strand.particleCount);
                // The sway phase of the LANE running this pass (m_time for the
                // scene, m_previewTime for the preview stage), passed by value,
                // never read from a member.
                const float w = 6.2831853f * freq * swayTime;
                for (uint32_t i = 0; i < strand.particleCount; ++i)
                {
                    const uint32_t pIdx = strand.particleOffset + i;
                    const float t = static_cast<float>(i) / (chainLen - 1.f);
                    // Deterministic per-bone phase (Knuth multiplicative hash):
                    // stable across reloads, unlike a particle-index hash, which
                    // would shift when an exclude list changes.
                    const uint32_t h = data.particles[pIdx].boneIndex * 2654435761u;
                    const float phase = turb * 6.2831853f *
                        (static_cast<float>(h >> 8) * (1.f / 16777216.f));
                    const float a = amp * t * std::sinf(w + phase);
                    data.windOffsetScratch[pIdx] = { windUnit.x * a, windUnit.y * a, windUnit.z * a };
                }
            }
        }

        // ---- World-motion inertia (worldInertiaLocation / Rotation) ----
        // The ENTITY moved this pass (PrepareWorldKick): conserve every free
        // particle's WORLD velocity across it (FeltKick). The kick goes into the
        // Verlet history only (prevPosition moves, position does not), so it is a
        // velocity change the authored damping, maxVelocity clamp and constraints
        // shape like any other motion; nothing teleports and nothing can be pushed
        // through a collider in one jump. Once per pass, before the substeps
        // (PrepareSubsteps has already set the step the Verlet displacement is
        // measured in). Pinned roots / frozen strands are kinematic and skip it.
        // Null worldKick: no kick at all.
        if (worldKick)
        {
            const float step = data.velocityStepSeconds;
            for (uint32_t si = isl.strandBegin; si < isl.strandEnd && step > 0.f; ++si)
            {
                const Strand& strand = data.strands[si];
                if (strand.frozen) continue;
                // The group's OWN share (not ovrEnabled-gated), else the component's.
                const ChainGroupDef* gd = strand.groupIdx < cfg.groups.size()
                                        ? &cfg.groups[strand.groupIdx] : nullptr;
                FeltKick k;
                if (!k.Resolve(worldKick->rot, worldKick->pos, worldKick->dt,
                               worldKick->prevRot, worldKick->prevPos, worldKick->prevDt,
                               EffectiveWorldInertia(gd, cfg, false),
                               EffectiveWorldInertia(gd, cfg, true)))
                    continue;
                for (uint32_t i = 0; i < strand.particleCount; ++i)
                {
                    Particle& p = data.particles[strand.particleOffset + i];
                    if (p.invMass == 0.f) continue;
                    const XMVECTOR x    = XMLoadFloat3(&p.position);
                    const XMVECTOR prev = XMLoadFloat3(&p.prevPosition);
                    const XMVECTOR kick = XMVectorScale(k.Delta(x), step);   // per-step displacement
                    // Translation only: the history just shifts by the kick, so a
                    // zero kick (constant entity velocity) leaves it bit-exact.
                    // With rotation the displacement (the Verlet velocity) is also
                    // re-expressed in the new orientation.
                    const XMVECTOR np = k.rot
                        ? XMVectorSubtract(x, XMVectorAdd(k.Turn(XMVectorSubtract(x, prev)), kick))
                        : XMVectorSubtract(prev, kick);
                    XMStoreFloat3(&p.prevPosition, np);
                }
            }
        }

        // ---- Motion-response scaling ----
        // Carry the chain along with (1 - inertiaScale) of the root bone's motion
        // this frame so it only FEELS the remaining fraction as relative
        // displacement. position AND prevPosition move together, a rigid translation
        // that injects no velocity: the chain gets quieter without getting stiffer,
        // which damping / stiffness alone cannot do.
        //
        // A positional carry is substep-count independent, so this runs once per
        // frame (before the substep loop) and needs no 1/nSub scaling. It also
        // absorbs entity teleports for free at low inertia.
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
        {
            const Strand& strand = data.strands[si];
            if (strand.particleCount < 2) continue;
            if (strand.frozen) continue;   // frozen by a preview solo: kinematic (see the substep root loop)
            const ChainGroupDef* o = grpOvr(strand.groupIdx);
            const bool isSkirt = (strand.type == StrandType::Skirt);
            float inertia = o ? o->ovrInertiaScale
                              : (isSkirt ? cfg.skirtInertiaScale : cfg.inertiaScale);
            inertia = std::min(std::max(inertia, 0.f), 1.f);
            if (inertia >= 1.f) continue;

            const uint32_t rootIdx = strand.particleOffset;
            // poseStartScratch[root] is last frame's animated root position (the
            // restPosition committed at the end of the previous Simulate), so this
            // delta is exactly the root's motion over the frame.
            const XMFLOAT3 rootNow  = cachedBonePos(data.particles[rootIdx].boneIndex);
            const XMFLOAT3& rootWas = data.poseStartScratch[rootIdx];
            const float carry = 1.f - inertia;
            const float cx = (rootNow.x - rootWas.x) * carry;
            const float cy = (rootNow.y - rootWas.y) * carry;
            const float cz = (rootNow.z - rootWas.z) * carry;
            if (cx == 0.f && cy == 0.f && cz == 0.f) continue;

            for (uint32_t i = 1; i < strand.particleCount; ++i)
            {
                Particle& p = data.particles[strand.particleOffset + i];
                p.position.x     += cx; p.position.y     += cy; p.position.z     += cz;
                p.prevPosition.x += cx; p.prevPosition.y += cy; p.prevPosition.z += cz;
            }
        }

        // Collider interpolation: with last frame's capsules (same count), advance
        // the collider from prev -> current across the substeps so a fast body bone
        // sweeps THROUGH the cloth instead of teleporting past it. First frame /
        // count change => no interpolation (static current pose).
        // interpCapsStart = the collider at the START of the substep: the tunneling
        // side test in SolveCapsuleCollision measures the particle's entry side
        // against it, catching a fast capsule that steps past a slow particle.
        WorldCapsule interpCaps[CapsuleColliderComponent::MAX_CAPSULES];
        WorldCapsule interpCapsStart[CapsuleColliderComponent::MAX_CAPSULES];

        for (int sub = 0; sub < nSub; ++sub)
        {
            const bool isLastSub = (sub == nSub - 1);
            const float subAlpha = static_cast<float>(sub + 1) / static_cast<float>(nSub);
            const float subAlphaPrev = static_cast<float>(sub) / static_cast<float>(nSub);

            // Kinematic roots follow the same interpolated animation pose as the
            // colliders instead of teleporting to the end-of-frame pose on substep 0.
            for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            {
                const Strand& strand = data.strands[si];
                // A strand FROZEN AT REST by a preview-lane solo is kinematic
                // end to end: EVERY particle rides the same interpolated
                // animated pose as the roots (pinned: invMass 0, so the
                // integration, the PBD and the contact passes leave it), and
                // it is snapped onto the exact pose before WriteBonePoses.
                if (strand.frozen)
                {
                    for (uint32_t i = 0; i < strand.particleCount; ++i)
                    {
                        const uint32_t pIdx = strand.particleOffset + i;
                        Particle& fp = data.particles[pIdx];
                        const XMFLOAT3 ft = cachedBonePos(fp.boneIndex);
                        const XMFLOAT3 fs = data.poseStartScratch[pIdx];
                        fp.position = { fs.x + (ft.x - fs.x) * subAlpha,
                                        fs.y + (ft.y - fs.y) * subAlpha,
                                        fs.z + (ft.z - fs.z) * subAlpha };
                        fp.prevPosition = fp.position;
                    }
                    continue;
                }
                const uint32_t rootIdx = strand.particleOffset;
                Particle& root = data.particles[rootIdx];
                const XMFLOAT3 target = cachedBonePos(root.boneIndex);
                const XMFLOAT3 start  = data.poseStartScratch[rootIdx];
                root.position = {
                    start.x + (target.x - start.x) * subAlpha,
                    start.y + (target.y - start.y) * subAlpha,
                    start.z + (target.z - start.z) * subAlpha
                };
                root.prevPosition = root.position;
            }

            // Interpolate the collider pose before solving this substep. The last
            // substep always uses the exact current-frame capsules.
            const WorldCapsule* subCaps      = capsules;
            const WorldCapsule* subCapsStart = nullptr;
            if (hasPrevCaps)
            {
                for (int c = 0; c < capsuleCount; ++c)
                {
                    const WorldCapsule& pc = data.prevCapsules[c];
                    const WorldCapsule& cc = capsules[c];
                    interpCaps[c].a = { pc.a.x + (cc.a.x - pc.a.x)*subAlpha,
                                        pc.a.y + (cc.a.y - pc.a.y)*subAlpha,
                                        pc.a.z + (cc.a.z - pc.a.z)*subAlpha };
                    interpCaps[c].b = { pc.b.x + (cc.b.x - pc.b.x)*subAlpha,
                                        pc.b.y + (cc.b.y - pc.b.y)*subAlpha,
                                        pc.b.z + (cc.b.z - pc.b.z)*subAlpha };
                    interpCaps[c].radius = cc.radius;

                    interpCapsStart[c].a = { pc.a.x + (cc.a.x - pc.a.x)*subAlphaPrev,
                                             pc.a.y + (cc.a.y - pc.a.y)*subAlphaPrev,
                                             pc.a.z + (cc.a.z - pc.a.z)*subAlphaPrev };
                    interpCapsStart[c].b = { pc.b.x + (cc.b.x - pc.b.x)*subAlphaPrev,
                                             pc.b.y + (cc.b.y - pc.b.y)*subAlphaPrev,
                                             pc.b.z + (cc.b.z - pc.b.z)*subAlphaPrev };
                    interpCapsStart[c].radius = cc.radius;
                }
                subCaps      = interpCaps;
                subCapsStart = interpCapsStart;
            }

            for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            {
                const Strand& strand = data.strands[si];
                if (strand.frozen) continue;   // frozen: kinematic, never integrated
                const bool  isSkirt = (strand.type == StrandType::Skirt);
                const ChainGroupDef* o = grpOvr(strand.groupIdx);
                // Clamp to [0,1] BEFORE the substep root: a negative authored
                // damping makes powf() return NaN for any nSub > 1 (and CCD raises
                // nSub on its own), which silently poisons the entire island; even
                // at nSub == 1 it sign-flips the Verlet velocity.
                const float baseDamp = std::min(std::max(
                    o ? o->ovrDamping : (isSkirt ? cfg.skirtDamping : cfg.damping), 0.f), 1.f);
                const float damp     = ChainPhysicsMath::Retention(baseDamp, subDt);
                const float grav     = o ? o->ovrGravity
                                         : (isSkirt ? cfg.skirtGravity : cfg.gravity);
                const float maxV     = isLastSub
                    ? (o ? o->ovrMaxVelocity
                         : (isSkirt ? cfg.skirtMaxVelocity : cfg.maxVelocity))
                    : 0.f;

                XMFLOAT3 gravity = { 0.f, grav, 0.f };
                // The preview SCENE's gravity (preview lane only, SetPreviewGravity)
                // turns the authored signed-Y magnitude into down * (-grav). Null on
                // every main-lane job, so the scene integrates the authored scalar
                // path, byte for byte.
                if (gravityDown)
                    gravity = { gravityDown->x * -grav, gravityDown->y * -grav,
                                gravityDown->z * -grav };
                IntegrateStrand(data, strand, subDt, damp, gravity, maxV * (60.f * subDt));
            }

            // KawaiiPhysics stiffness target: simulated parent + animated local
            // parent->child delta. Pulling each point toward an absolute world-space
            // animation position makes every descendant fight body collision and
            // root motion independently. The parent-relative target preserves the
            // authored silhouette while the whole chain deflects coherently.
            for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            {
                const Strand& strand = data.strands[si];
                if (strand.frozen) continue;   // frozen: kinematic, no local pull
                const bool isSkirt = (strand.type == StrandType::Skirt);
                const ChainGroupDef* o = grpOvr(strand.groupIdx);
                // Same gate the blend precompute above used: strands that skip
                // it have no valid localBlendScratch entries to read.
                const float ls = o ? o->ovrLocalStiffness
                                   : (isSkirt ? cfg.skirtLocalStiffness : cfg.localStiffness);
                if (ls <= 0.f || strand.particleCount < 2) continue;

                for (uint32_t i = 1; i < strand.particleCount; ++i)
                {
                    const uint32_t pIdx = strand.particleOffset + i;
                    const uint32_t parentIdx = pIdx - 1;
                    Particle& p = data.particles[pIdx];
                    const Particle& parent = data.particles[parentIdx];
                    const XMFLOAT3 currentRest = cachedBonePos(p.boneIndex);
                    const XMFLOAT3 currentParentRest = cachedBonePos(parent.boneIndex);
                    const XMFLOAT3& startRest = data.poseStartScratch[pIdx];
                    const XMFLOAT3& startParentRest = data.poseStartScratch[parentIdx];
                    const XMFLOAT3 restWorld = {
                        startRest.x + (currentRest.x - startRest.x) * subAlpha,
                        startRest.y + (currentRest.y - startRest.y) * subAlpha,
                        startRest.z + (currentRest.z - startRest.z) * subAlpha
                    };
                    const XMFLOAT3 parentRestWorld = {
                        startParentRest.x + (currentParentRest.x - startParentRest.x) * subAlpha,
                        startParentRest.y + (currentParentRest.y - startParentRest.y) * subAlpha,
                        startParentRest.z + (currentParentRest.z - startParentRest.z) * subAlpha
                    };
                    XMFLOAT3 target = {
                        parent.position.x + (restWorld.x - parentRestWorld.x),
                        parent.position.y + (restWorld.y - parentRestWorld.y),
                        parent.position.z + (restWorld.z - parentRestWorld.z)
                    };

                    // Ambient sway rides the same parent-relative target, so only
                    // the DIFFERENCE across the link matters (a whole-strand offset
                    // would cancel; see the pre-pass).
                    if (anyWind)
                    {
                        const XMFLOAT3& wc = data.windOffsetScratch[pIdx];
                        const XMFLOAT3& wp = data.windOffsetScratch[parentIdx];
                        target.x += wc.x - wp.x;
                        target.y += wc.y - wp.y;
                        target.z += wc.z - wp.z;
                    }

                    // The target must not command a position inside a collider
                    // (see declipTarget above). Uses this substep's interpolated
                    // capsules, the same ones the contact passes below see.
                    if (hasCapsules)
                        declipTarget(target, p.radius, subCaps, capsuleCount,
                                     contactSkin);

                    const float blend = data.localBlendScratch[pIdx];

                    p.position.x += (target.x - p.position.x) * blend;
                    p.position.y += (target.y - p.position.y) * blend;
                    p.position.z += (target.z - p.position.z) * blend;
                }
            }

            // Continuous node collision is needed only once after integration.
            // It also applies contact friction once per substep.
            if (hasCapsules)
                SolveCapsuleCollision(data, isl, subCaps, capsuleCount, subCapsStart, true, true, contactSkin);

            // See Chain.Collide: this zone's SELF time (total minus the nested
            // Chain.Collide calls) is the pure PBD constraint-projection cost.
            ZoneScopedN("Chain.Solve");
            // LOD iteration count, clamped to >= 1 by the caller. At 0 this loop
            // would skip both the interleaved ApplyRestLimit and the
            // per-iteration static collision, leaving only the swept pass:
            // cloth could finish the frame inside the body.
            for (int iter = 0; iter < iterations; ++iter)
            {
                for (int g = 0; g < data.groupCount; ++g)
                {
                    for (uint32_t ci = isl.constraintBegin[g]; ci < isl.constraintEnd[g]; ++ci)
                    {
                        const Constraint& c = data.constraintGroups[g][ci];
                        Particle& pA = data.particles[c.particleA];
                        Particle& pB = data.particles[c.particleB];

                        float dx = pB.position.x - pA.position.x;
                        float dy = pB.position.y - pA.position.y;
                        float dz = pB.position.z - pA.position.z;
                        float dist = std::sqrtf(dx*dx + dy*dy + dz*dz);
                        if (dist < 1e-6f) continue;

                        float error = (dist - c.restLength) / dist;

                        // Bucket stiffness x per-group override x root->tip decay,
                        // clamped and substep-compensated once per frame above.
                        const float stiffness = data.constraintStiffScratch[g][ci];

                        // Mass-weighted PBD: a pinned neighbour (invMass 0) hands the
                        // FULL correction to the free particle, so the chain does not
                        // lag / stretch off a fast-moving root; two free particles
                        // split it 0.5 / 0.5.
                        float wA = pA.invMass, wB = pB.invMass;
                        float wSum = wA + wB;
                        if (wSum < 1e-8f) continue;
                        float corr = error * stiffness / wSum;
                        float cx = dx * corr;
                        float cy = dy * corr;
                        float cz = dz * corr;

                        if (pA.invMass > 0.f) { pA.position.x += cx * wA; pA.position.y += cy * wA; pA.position.z += cz * wA; }
                        if (pB.invMass > 0.f) { pB.position.x -= cx * wB; pB.position.y -= cy * wB; pB.position.z -= cz * wB; }
                    }
                }

                // The rest limit is a constraint like any other and projects on
                // EVERY iteration, for the same reason contact does: applied once
                // after the whole solve, its rotation keeps the vertical link's
                // length but breaks the distance to the NEIGHBOURING chain (the
                // horizontal / shear links of a cloth ring), and nothing repairs
                // them afterwards, so a skirt comes out kinked. No velocity
                // response here; that happens once, on the final call, or the
                // damping would scale with iterations. It receives this substep's
                // colliders so a cone whose target sits inside one skips the
                // clamp instead of fighting the contact pass.
                ApplyRestLimit(data, isl, cfg, skel, subAlpha, false,
                               hasCapsules ? subCaps : nullptr,
                               hasCapsules ? capsuleCount : 0, contactSkin);

                // Contact is the final projection of every PBD iteration, so distance,
                // horizontal and shear constraints converge around the body instead of
                // getting one chance to pull the result back through it. It also stays
                // AFTER the rest limit, so pulling cloth toward its rest cone never
                // wins over not being inside the body. Tangential velocity is kept here;
                // repeated friction would make its strength depend on cfg.iterations.
                // The collider is static within the iteration loop, so no substep-start
                // pose is passed: the side test runs against the current capsule.
                if (hasCapsules)
                    SolveCapsuleCollision(data, isl, subCaps, capsuleCount, nullptr, false, false, contactSkin);
            }

        }

        // Commit the true current animation pose for next frame's target
        // interpolation, including roots and groups whose local stiffness is zero.
        for (uint32_t i = isl.particleBegin; i < isl.particleEnd; ++i)
        {
            Particle& p = data.particles[i];
            p.restPosition = cachedBonePos(p.boneIndex);
        }

        // ---- Divergence safeguard ----
        // The PBD distance projection bypasses the velocity clamp, so a corrupted
        // rest length or a bad early-frame pose (a reloaded character whose
        // recycled-id pose is not ready yet) can fling particles thousands of units
        // from their bone until the skinned vertices span the screen and the GPU
        // hangs (TDR). Bound every particle to the strand's reach (sqrt(maxReachSq):
        // 2x its rest extent + 0.05, from InitEntity) around the bone-pinned strand
        // root: a runaway particle snaps back onto that sphere, a NaN one onto its
        // bone. Scale-relative, so legit chains of any size are untouched.
        for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
        {
            const Strand& strand = data.strands[si];
            if (strand.particleCount < 2 || strand.maxReachSq <= 0.f) continue;
            const XMFLOAT3 rootPos = data.particles[strand.particleOffset].position;
            for (uint32_t i = 1; i < strand.particleCount; ++i)
            {
                Particle& p = data.particles[strand.particleOffset + i];
                const float dx = p.position.x - rootPos.x;
                const float dy = p.position.y - rootPos.y;
                const float dz = p.position.z - rootPos.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 <= strand.maxReachSq) continue;   // in bounds (and not NaN)
                const float d = std::sqrtf(d2);
                if (d > 1e-6f && d == d)                 // finite: pull back onto the sphere
                {
                    const float s = std::sqrtf(strand.maxReachSq) / d;
                    p.position = { rootPos.x + dx * s, rootPos.y + dy * s, rootPos.z + dz * s };
                }
                else                                     // NaN / degenerate: reset to the bone
                {
                    p.position = cachedBonePos(p.boneIndex);
                }
                p.prevPosition = p.position;             // kill the runaway Verlet velocity
                clampedDiverged = true;
            }
        }
        // Divergence LOGGING happens in RunPass's serial epilogue (via the
        // return value): divergenceLogged is entity-wide state.

        // Final rest-limit projection. The cone was already enforced inside every
        // PBD iteration above (so the distance / ring constraints converged
        // around it); this last call guarantees the pose actually written to the
        // bones respects it after the divergence clamp, and is the one call that
        // applies the velocity response. Deliberately BEFORE the final capsule
        // pass: pulling cloth back toward rest can push it into the body, and
        // that pass is what repairs it.
        ApplyRestLimit(data, isl, cfg, skel, 1.f, true,
                       hasCapsules ? capsules : nullptr,
                       hasCapsules ? capsuleCount : 0, contactSkin);

        // Final static contact pass also repairs the rare case where the divergence
        // safeguard or the rest-limit above moved a particle back onto a point
        // inside a capsule.
        if (hasCapsules)
            SolveCapsuleCollision(data, isl, capsules, capsuleCount, nullptr, false, false, contactSkin);

        // Frozen strands end the pass EXACTLY on this frame's animated pose
        // (== restPosition, committed above) with no velocity, so WriteBonePoses
        // aims every frozen bone along its animated direction (the clip's
        // rotation). Whatever the solve did near them (a rest-limit rotation, the
        // divergence clamp) is undone here. On the main lane no strand is ever
        // frozen (frozenStrands == 0).
        if (data.frozenStrands != 0)
            for (uint32_t si = isl.strandBegin; si < isl.strandEnd; ++si)
            {
                const Strand& strand = data.strands[si];
                if (!strand.frozen) continue;
                for (uint32_t i = 0; i < strand.particleCount; ++i)
                {
                    Particle& fp = data.particles[strand.particleOffset + i];
                    fp.position     = cachedBonePos(fp.boneIndex);
                    fp.prevPosition = fp.position;
                }
            }

        WriteBonePoses(data, isl, poses, skel);
    }

    // ---- Spring bones (jiggle physics) ----
    if (isl.springs && !data.springBones.empty())
    {
        SimulateSpringBones(data, cfg, cdt, poses, skel, gravityDown, worldKick);
        WriteSpringBonePoses(data, poses, skel);
    }

    // data.prevCapsules is NOT updated here: islands of one entity read it
    // concurrently. RunPass's serial epilogue commits it once per entity after
    // all island jobs joined.
    return clampedDiverged;
}

// ==== ApplyRestPoseOnly: the Edit / Pause path ====
// The same pool walk and guards as RunPass's phase 1, minus everything that is
// simulation: no InitEntity, no m_entityData, no integration.
void ChainPhysicsSystem::ApplyRestPoseOnly(World& world,
                                            const std::unordered_set<Entity>* activeSet,
                                            const std::unordered_set<Entity>* exclude)
{
    ZoneScopedN("ChainPhysics.RestPoseOnly");

    auto* pPhys = world.GetPool<ChainPhysicsComponent>();
    auto* pSkel = world.GetPool<SkeletonComponent>();

    // ---- Restore bones driven LAST frame but not this one ----
    // Without this, Edit-mode authoring is a one-way valve: this path never runs
    // resetRot and AnimationSystem seeds a source-less skeleton once, so the last
    // rotation written would stay in poses[b].rot forever.
    //
    // A lambda used on EVERY exit route, not a tail block of the loop body: the
    // guards below (component disabled, no skeleton, no pose slot) and the
    // "entity left the pool" case (component removed / entity destroyed) must
    // ALL restore, or un-ticking ChainPhysicsComponent::enabled would freeze the
    // authored curl on screen with the component visibly off.
    std::vector<uint32_t> driven;
    auto reconcile = [&](Entity e, const SkeletonAsset* skel,
                         AnimationSystem::LocalPose* poses)
    {
        auto prevIt = m_editRestPoseApplied.find(e);
        if (prevIt != m_editRestPoseApplied.end() && skel && poses)
        {
            for (uint32_t bn : prevIt->second)
            {
                if (bn >= skel->boneCount) continue;
                if (std::binary_search(driven.begin(), driven.end(), bn)) continue;
                XMVECTOR sc, rq, tr;
                if (XMMatrixDecompose(&sc, &rq, &tr, XMLoadFloat4x4(&skel->restPoseLocal[bn])))
                    XMStoreFloat4(&poses[bn].rot, XMQuaternionNormalize(rq));
            }
        }
        if (driven.empty()) m_editRestPoseApplied.erase(e);
        else                m_editRestPoseApplied[e] = driven;
    };

    // Entities visited below; anything left in the map afterwards lost its
    // component (or its entity) and still owes a restore.
    std::unordered_set<Entity> visited;

    const size_t physN = pPhys ? pPhys->Data().size() : 0;
    const auto&  physEnts = EntitiesOf(pPhys);
    std::uint32_t excludedHere = 0;   // counted into m_laneStats.excludedByPreviewLane
    for (size_t pi = 0; pi < physN; ++pi)
    {
        const Entity e = physEnts[pi];
        // A culled entity is NOT reconciled: nothing wrote its pose this frame
        // either, so its recorded bones stay valid until it comes back.
        if (activeSet && activeSet->find(e) == activeSet->end()) { visited.insert(e); continue; }
        // The preview lane owns this entity. It is VISITED like the culled entity
        // above, NEVER skipped with a bare `continue`: the orphan sweep at the
        // bottom reconciles every unvisited record, which would restore this
        // entity's authored rest rotations to restPoseLocal and ERASE its record
        // every Edit-mode frame while the preview lane stamps them back. Silent
        // unless restPoseWeight < 1, so it is counted (restPoseRecordChurn) and
        // asserted by the chain_preview_sim_held pack and the chain_preview_lane
        // unit test. Any skip added to this loop follows the same rule.
        if (exclude && exclude->count(e))
        {
            ++excludedHere;
#if WITH_HARNESS
            // -chainpreview_visit_off: the REVERSE ARM of chain_preview_sim_held,
            // and nothing else. It re-creates the bare-continue bug so the pack
            // can show restPoseRecordChurn going red; without an arm that moves
            // it, a "max 0" on that counter proves nothing.
            static const bool s_visitOff =
                Sys::CommandLine::Get().Has("-chainpreview_visit_off");
            if (!s_visitOff)
#endif
                visited.insert(e);
            continue;
        }
        visited.insert(e);

        auto* phys = &pPhys->Data()[pi];

        auto* skelComp = pSkel ? pSkel->Get(e) : nullptr;
        const SkeletonAsset* skel =
            (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
                ? &m_skeletons.Get(skelComp->assetIndex) : nullptr;
        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);

        driven.clear();
        // `enabled` gates only the APPLY. The restore below still has to run, or
        // the checkbox cannot undo what it turned on.
        if (phys->enabled && skel && poses)
        {
            ApplyRestPose(*phys, *skel, skelComp->assetIndex, poses);
            for (const BoneRestPose& r : phys->restPose)
                if (r.appliedThisFrame && r.cachedBoneIdx < skel->boneCount)
                    driven.push_back(r.cachedBoneIdx);
            std::sort(driven.begin(), driven.end());
            driven.erase(std::unique(driven.begin(), driven.end()), driven.end());
        }
        else if (!phys->enabled)
        {
            // Stale appliedThisFrame flags would keep RunPass's resetRot skipping
            // these bones the moment the transport starts again.
            for (BoneRestPose& r : phys->restPose) r.appliedThisFrame = false;
        }

        // restPoseRecordChurn: a preview-set entity's record is created ONCE per
        // stage; a second creation means something erased it in between (the
        // bare-continue failure above). One pointer test per entity when no
        // preview set is installed.
        const bool trackRecord = m_previewSet && m_previewSet->count(e);
        const bool hadRecord   = trackRecord && m_editRestPoseApplied.count(e);
        reconcile(e, skel, poses);
        if (trackRecord && !hadRecord && m_editRestPoseApplied.count(e) &&
            !m_previewRecordSeen.insert(e).second)
            ++m_laneStats.restPoseRecordChurn;
    }
    if (exclude) m_laneStats.excludedByPreviewLane += excludedHere;

    // Component removed / entity destroyed between frames: the pool loop never
    // saw it, so restore from the world directly and drop the record.
    if (!m_editRestPoseApplied.empty())
    {
        std::vector<Entity> orphans;
        for (const auto& kv : m_editRestPoseApplied)
            if (visited.find(kv.first) == visited.end()) orphans.push_back(kv.first);
        for (Entity e : orphans)
        {
            // A LIVE preview entity reaching this sweep is exactly the bug the
            // visited rule above prevents (a destroyed one never gets here:
            // OnEntityDestroyed drops its record first).
            if (m_previewSet && m_previewSet->count(e)) ++m_laneStats.restPoseRecordChurn;
            const SkeletonComponent* skelComp =
                world.IsAlive(e) ? world.GetComponent<SkeletonComponent>(e) : nullptr;
            const SkeletonAsset* skel =
                (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
                    ? &m_skeletons.Get(skelComp->assetIndex) : nullptr;
            AnimationSystem::LocalPose* poses =
                world.IsAlive(e) ? m_animSys.GetMutableLocalPose(e) : nullptr;
            driven.clear();
            reconcile(e, skel, poses);   // driven empty => restores all, then erases
        }
    }
}

// ==== Update: the per-frame entry point (main lane, then preview lane) ====
void ChainPhysicsSystem::Update(World& world, float dt,
                                 const std::unordered_set<Entity>* activeSet)
{
    ZoneScopedN("ChainPhysics");
    ++m_updateSerial;
    m_laneStats.excludedByPreviewLane = 0;
#if WITH_HARNESS
    // -chainpreview_world_off: the REVERSE ARM of the world-inertia packs (the
    // -chainpreview_poke_ttl_off pattern) - both lanes solve as if every
    // world-inertia field were 0, so a pack's "the entity's motion now reaches
    // the chains" gate must go red with the solver feature and nothing else off.
    static const bool s_worldOff = Sys::CommandLine::Get().Has("-chainpreview_world_off");
    if (s_worldOff && !m_worldInertiaSuppressed)
    {
        m_worldInertiaSuppressed = true;
        LOG_INFO("[CHAINPHYS] -chainpreview_world_off: world-motion inertia suppressed "
                 "(reverse arm)");
    }
#endif

    // ---- MAIN lane: the scene ----
    // The preview set (null unless the Chain Physics Preview panel is open) is
    // EXCLUDED in both branches, so the scene's pass never touches an entity the
    // preview lane owns.
    //
    // Renderer::BeginFrame also runs in the editor's Edit and Paused modes so
    // skinning can still draw. Chain physics is simulation, however, and must
    // not initialize state or write LocalPose until the transport advances.
    //
    // The authored rest pose is the one exception: it is AUTHORING DATA, not
    // simulation, and it has to reach the mesh in Edit / Pause or dragging the
    // viewport pose gizmo would show nothing at all (AnimationSystem,
    // LocalToWorld and skinning all still run here; only this system is gated).
    if (!m_simulationEnabled && m_hold)
    {
        // Time stands still while the scene plays: hold the simulated pose (RunPass's hold jobs).
        RunPass(world, 0.f, activeSet, m_previewSet, m_time, Lane::Main);
    }
    else if (!m_simulationEnabled)
    {
        ApplyRestPoseOnly(world, activeSet, m_previewSet);
    }
    // Zero / invalid elapsed time must not consume or erase Verlet history:
    // with the transport running at dt == 0 the main lane does NOTHING, not even
    // the rest pose.
    else if (dt > 0.f && std::isfinite(dt))
    {
        // Ambient-sway clock. Advanced here in the serial phase so the parallel
        // island jobs see one stable value. Wrapped an hour in: sinf's argument grows
        // as 2*pi*freq*t, and letting it run unbounded eventually costs enough
        // mantissa to make the sway visibly step. The wrap does cost one imperceptible
        // phase jump per hour, which is the cheaper of the two artifacts.
        m_time += dt;
        if (m_time > 3600.f) m_time -= 3600.f;
        RunPass(world, dt, activeSet, m_previewSet, m_time, Lane::Main);
        // -chainlodshadowrate (or LodTiers::shadowOnlyHalfRate on): every 120 updates,
        // log the half-rate holds per update.
        static const bool s_rateLog = Sys::CommandLine::Get().Has("-chainlodshadowrate");
        if ((s_rateLog || lodTiers.shadowOnlyHalfRate) && m_updateSerial % 120 == 0)
        {
            LOG_INFO("[CHAINLOD] shadowRate holdsPerUpdate=%.2f lodEnabled=%d",
                     static_cast<double>(m_rateHolds) / 120.0, lodEnabled ? 1 : 0);
            m_rateHolds = 0;
        }
    }

    // ---- PREVIEW lane ----
    // SERIAL, after the main lane, never beside it: RunPass's island dispatch is
    // a flat ParallelFor, and two passes in flight would nest ParallelFor on a
    // TaskSystem that does not work-steal. Its own dt and its OWN sway phase
    // (m_previewTime): in Edit mode the main lane never advances m_time.
    if (m_previewSet && !m_previewSet->empty())
    {
        ZoneScopedN("ChainPhysics.PreviewLane");
        ++m_laneStats.frames;
        m_laneStats.steps = 0;
        // Every A/B shadow's input = its source's animated input of THIS
        // frame, copied before any preview pass (or hold) writes the source's
        // pose. The main lane above never touches a preview entity.
        if (!m_poseFeeds.empty()) FeedPreviewPoses(world);
        if (m_previewDt > 0.f && std::isfinite(m_previewDt) && m_previewSteps == 0)
        {
            // HOLD: the lane is live but this editor frame owns no fixed step
            // (the StageClock accumulated less than one, or the panel is
            // paused). AnimationSystem has just re-sampled the raw clip pose;
            // re-write the last simulated rotations over it from the particles
            // as they are, or the chains flicker to the clip pose on every
            // step-less frame. No integration, no InitEntity, no clock.
            // The INPUT is kept first: it is the next stepping frame's
            // interpolation origin (the playhead has not moved: same pose).
            SnapshotPreviewPoses(world, *m_previewSet);
            HoldPass(world, *m_previewSet);
            KeepPreviewInputAsPrevious();
            ++m_laneStats.holdFrames;
        }
        else if (m_previewDt > 0.f && std::isfinite(m_previewDt))
        {
            // `steps` fixed steps this frame, SERIAL (never a ParallelFor around
            // RunPass: its island dispatch is one already). The frame's input pose
            // is kept (snapshot) and every pass gets its target written before it
            // runs: pass k < n the input interpolated k/n from the previous preview
            // frame's (the targets a faster editor would have sampled), pass n this
            // frame's exactly. Without the rewrite pass k would read pass k-1's
            // SIMULATED rotations as its animated target (a clip-driven skeleton
            // runs no resetRot) and chase itself. n == 1 writes nothing: one plain
            // RunPass (the snapshot only feeds the next frame's interpolation).
            const std::uint32_t n = std::min(m_previewSteps, kMaxPreviewStepsPerFrame);
            SnapshotPreviewPoses(world, *m_previewSet);
            float serialMs = 0.f, parallelMs = 0.f, cpuMs = 0.f;
            for (std::uint32_t k = 0; k < n; ++k)
            {
                if (k + 1 < n)
                {
                    if (WriteInterpolatedPreviewPoses(static_cast<float>(k + 1) /
                                                      static_cast<float>(n)) != 0u)
                        ++m_laneStats.interpolatedPasses;
                }
                else if (k > 0)
                    RestorePreviewPoses();
                m_previewTime += m_previewDt;
                if (m_previewTime > 3600.f) m_previewTime -= 3600.f;
                ++m_laneStats.passes;
                // World-motion inertia: the entity transform is written once per
                // editor frame, so pass k of n moves it 1/(n-k) of the remaining
                // way (the same spread the input pose gets above).
                m_passRemaining = n - k;
                RunPass(world, m_previewDt, m_previewSet, /*exclude=*/nullptr,
                        m_previewTime, Lane::Preview);
                m_passRemaining = 1;
                const Anim::AnimProfile& ap = Anim::AnimProfile::Get();
                serialMs   += ap.previewChainSerialMs;
                parallelMs += ap.previewChainParallelMs;
                cpuMs      += ap.previewChainCpuMs;
            }
            // The preview ms fields are the FRAME's lane cost (every pass of it),
            // written only here, by the preview lane. n == 1 stores the single
            // pass's values unchanged (0 + x == x).
            Anim::AnimProfile& ap = Anim::AnimProfile::Get();
            ap.previewChainSerialMs   = serialMs;
            ap.previewChainParallelMs = parallelMs;
            ap.previewChainCpuMs      = cpuMs;
            m_laneStats.serialMs   = serialMs;
            m_laneStats.parallelMs = parallelMs;
            m_laneStats.cpuMs      = cpuMs;
            m_laneStats.steps      = n;
            m_laneStats.maxStepsPerFrame = std::max(m_laneStats.maxStepsPerFrame, n);
            KeepPreviewInputAsPrevious();
        }
        else
        {
            // PAUSED (dt 0): the preview set gets exactly what the main lane
            // gives the scene with the transport stopped: the authored rest
            // pose and nothing else. Without this the stage would get NEITHER
            // lane's treatment while paused (the main lane excludes it).
            // No interpolation origin survives a paused frame.
            m_previewPrevCount = 0;
            ApplyRestPoseOnly(world, m_previewSet, /*exclude=*/nullptr);
            ++m_laneStats.pausedFrames;
            m_laneStats.entities = 0;
            m_laneStats.substeps = 0;
        }
#if WITH_HARNESS
        // The periodic line is a harness artefact. An interactive session
        // never asks for it (every close still prints the closing summary),
        // so a panel left open does not write a line a second into the
        // user's log.
        if (m_previewLogEvery != 0u && m_laneStats.frames % m_previewLogEvery == 0u)
            LogPreviewLaneStats("periodic");
#endif
    }

#if WITH_HARNESS
    // -chainphys_statehash: one FNV-1a line per Update over the solver's whole
    // state (every initialised entity in id order: particles' position AND
    // prevPosition, springs' position AND velocity, the post-chain LocalPose).
    // Bit-identity claims are proved with this, not with a screenshot: a capture
    // has a same-binary pixel noise floor, a hash of the solver state has none.
    // Read-only.
    static const bool s_stateHash = Sys::CommandLine::Get().Has("-chainphys_statehash");
    if (s_stateHash)
    {
        std::vector<Entity> ids;
        ids.reserve(m_entityData.size());
        for (const auto& kv : m_entityData)
            if (kv.second.initialized) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        std::uint32_t h = 2166136261u;
        auto mix = [&h](const void* p, std::size_t n)
        {
            const auto* b = static_cast<const std::uint8_t*>(p);
            for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
        };
        std::uint64_t nParticles = 0, nSprings = 0;
        for (Entity e : ids)
        {
            const EntityData& d = m_entityData.at(e);
            mix(&e, sizeof(e));
            for (const Particle& p : d.particles)
            {
                mix(&p.position, sizeof(p.position));
                mix(&p.prevPosition, sizeof(p.prevPosition));
            }
            for (const SpringBone& sb : d.springBones)
            {
                mix(&sb.currentWorldPos, sizeof(sb.currentWorldPos));
                mix(&sb.velocity, sizeof(sb.velocity));
            }
            if (const AnimationSystem::LocalPose* lp = m_animSys.GetLocalPose(e))
                mix(lp, d.boneWorldMatCache.size() * sizeof(AnimationSystem::LocalPose));
            nParticles += d.particles.size();
            nSprings   += d.springBones.size();
        }
        LOG_INFO("[CHAINHASH] update=%llu entities=%zu particles=%llu springs=%llu hash=%08x",
                 static_cast<unsigned long long>(m_updateSerial), ids.size(),
                 static_cast<unsigned long long>(nParticles),
                 static_cast<unsigned long long>(nSprings), h);
    }
#endif
}

// ==== RunPass: ONE pass of the solver for one lane ====
// Phase 1 (serial) collects and initialises the entities, skipping `exclude`
// (the other lane's entities); phase 2 simulates the islands in parallel. The
// sway phase is the swayTime argument (Simulate takes it by value), the
// assign-style ms fields go to the lane's own set, and the lane bookkeeping
// counts excluded entities, entities solved by both lanes and the preview
// lane's entities / substeps. The visited-set rule for `exclude` lives in
// ApplyRestPoseOnly.
// Ordering contracts: ApplyRestPose before InitEntity and only on the serial
// phase-1 path; no ParallelFor nested in the island dispatch.
void ChainPhysicsSystem::RunPass(World& world, float dt,
                                 const std::unordered_set<Entity>* activeSet,
                                 const std::unordered_set<Entity>* exclude,
                                 float swayTime, Lane lane)
{
    const auto _chainT0 = std::chrono::steady_clock::now();
    // World-motion inertia: this lane's pass clock (EntityData::wmPass).
    ++m_lanePasses[static_cast<int>(lane)];

    // Phase 1 (single-threaded): collect + init entities. Serial because
    // InitEntity inserts into m_entityData (an unordered_map).
    struct PhysJob {
        Entity e;
        // The entity whose World data (capsules, transform) this job reads:
        // e itself, except for an A/B shadow (its feed's source).
        Entity src = NullEntity;
        ChainPhysicsComponent* phys;
        AnimationSystem::LocalPose* poses;
        const SkeletonAsset* skel;
        EntityData* data;
        WorldCapsule capsules[CapsuleColliderComponent::MAX_CAPSULES];
        int capsuleCount = 0;
        const CapsuleColliderComponent* capsuleSource = nullptr;
        XMFLOAT3 worldPosition{};
        bool shadowOnly = false;
        bool clamped = false;
        float lodDistance = 0.f;
        int nSub = 1;   // resolved before this entity's island solver
        // LOD tier output. Carried here rather than written into
        // ChainPhysicsComponent, which is authored, Inspector-visible and
        // serialized: a runtime quality decision must never land in it.
        int  iterations = 4;
        bool collide    = true;
        int  tier       = 0;
        // The preview SCENE, carried by Lane::Preview jobs only (set in phase 1
        // from the members SetPreviewGravity / SetPreviewSceneCapsules wrote in
        // the panel's serial window). A main-lane job keeps null / false, so the
        // scene's solve is unchanged.
        // The solver simulates in the SKELETON's own space (the bone cache is FK
        // from the LocalPose, no entity transform), while the preview scene is
        // authored in WORLD space, so phase 1 maps it through the inverse of the
        // entity's GlobalTransform: the gravity direction (magnitude kept) and,
        // in prepareEntity, the scene capsules (radius / scale).
        XMFLOAT3 gravitySkel{};
        bool gravityOvr = false;
        bool sceneCaps = false;
        int  sceneCapCount = 0;   // appended by prepareEntity (stats only)
        XMFLOAT4X4 invWorld{};    // world -> skeleton space (sceneCaps only)
        float invScale = 1.f;
        // World-motion inertia: this pass's entity-motion input, set in phase 1
        // (PrepareWorldKick) for a component that authors any. worldKickOn false =
        // Simulate gets nullptr: the solve without world inertia, byte for byte.
        WorldKick worldKick{};
        bool      worldKickOn = false;
        // LodTiers::shadowOnlyHalfRate: hold = this pass does not solve the
        // entity, it re-writes its bones from the particles as they are;
        // dt = the elapsed time this entity's solve integrates (RunPass's dt,
        // plus the carry of a held previous frame).
        bool  hold = false;
        float dt   = 0.f;
    };
    std::vector<PhysJob> jobs;
    // Reserved because the loops below and the island build hold references
    // and pointers INTO this vector (islandJobs stores &j), so it must not
    // reallocate once the push loop has finished.
    jobs.reserve(64);

    // Iterate the ChainPhysicsComponent pool directly: only a handful of
    // entities have physics chains, so this avoids the biggest serial scan of
    // the frame.
    auto* pPhys = world.GetPool<ChainPhysicsComponent>();
    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t physN = pPhys ? pPhys->Data().size() : 0;
    const auto&  physEnts = EntitiesOf(pPhys);
    std::uint32_t excludedHere = 0;   // counted into m_laneStats.excludedByPreviewLane
    for (size_t pi = 0; pi < physN; ++pi)
    {
        const Entity e = physEnts[pi];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        // The OTHER lane owns this entity this frame (the main lane passes the
        // preview set). Serial phase-1 read only: nothing past this loop, and
        // nothing in phase 2, looks at `exclude`.
        if (exclude && exclude->count(e)) { ++excludedHere; continue; }

        auto* phys = &pPhys->Data()[pi];
        if (!phys->enabled) continue;

        // An A/B SHADOW (SetPreviewPoseFeed, preview lane only) has no skeleton
        // or pose of its own: it is solved on its source's skeleton, from the
        // solver-owned copy of the source's input pose (FeedPreviewPoses), and
        // every other per-entity World read below (the pose-source verdict, the
        // GlobalTransform, the capsules) is the source's. src == e for every
        // other entity and on the main lane.
        const bool   fed = lane == Lane::Preview && !m_poseFeeds.empty() &&
                           FindPoseFeed(e) != nullptr;
        const Entity src = fed ? PreviewPoseSourceOf(e) : e;
        auto* skelComp = pSkel ? pSkel->Get(src) : nullptr;
        if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle) continue;

        AnimationSystem::LocalPose* poses = fed ? PreviewPoses(e) : m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        const SkeletonAsset& skel = m_skeletons.Get(skelComp->assetIndex);

        // Authored rest pose FIRST, before InitEntity and before the no-clip
        // resetRot cleanup:
        //  (1) InitEntity bakes Constraint::restLength, Strand::maxReachSq and
        //      SpringBone::restLocalPos from the LocalPose as it stands AT THAT
        //      MOMENT. Running after it would bake a cloth piece's ring / shear
        //      lengths from the UN-authored shape, and that piece would then
        //      fight its own new rest forever.
        //  (2) The FK anchor cache below has to see the authored rotations: it is
        //      the single source every rest reference in the solver derives from.
        // Unconditional: never gate this on a pose-source test. Gating would
        // inherit that test's blind spots (a short clip that only rewrites the
        // first n bones still reports "clip active"), and the authored pose is
        // authoring data that must land in every mode.
        ChainPhysicsSystem::ApplyRestPose(*phys, skel, skelComp->assetIndex, poses);

        auto it = m_entityData.find(e);
        const bool justInitialized = (it == m_entityData.end() || !it->second.initialized);   // also gates the half-rate hold below
        if (justInitialized)
        {
            InitEntity(e, *phys, skel, poses);
            EntityData& d = m_entityData[e];
            LOG_INFO("ChainPhysics: RUNNING entity %u — damping=%.3f gravity=%.1f maxVel=%.2f stiffness=%.2f iter=%d strands=%zu particles=%zu springs=%zu",
                     e, phys->damping, phys->gravity, phys->maxVelocity,
                     phys->structuralStiffness, phys->iterations,
                     d.strands.size(), d.particles.size(), d.springBones.size());
            // A preview-lane solo freezes the other groups AT REST, applied to
            // the state InitEntity just built (a solo change Invalidates, so it
            // always lands here). The main lane never freezes.
            if (lane == Lane::Preview) ApplySoloFreeze(e, d);
        }

        EntityData& data = m_entityData[e];
        if (data.strands.empty() && data.springBones.empty()) continue;
        // The pending pokes, resolved for THIS pass (serial: the island workers
        // only read extAccel). Preview lane only: a main-lane pass never
        // integrates an external force, and a queue left behind by an entity that
        // moved to the main lane is dropped, never consumed there.
        if (lane == Lane::Preview)
            data.extAccelActive = PrepareExternalImpulses(data, dt);
        else if (data.extAccelActive || !data.impulses.empty())
        {
            data.extAccelActive = false;
            data.impulses.clear();
        }

        // No live pose source: re-establish the clean base. The sim reads its
        // follow / anchor targets from poses via FK (ComputeBindWorldPos) and
        // WriteSpringBonePoses COMPOSES the jiggle onto poses[bi].rot, so both need
        // poses[b].rot to hold the clean base (rest / clip) this frame.
        // AnimationSystem re-samples a clip every frame, but WITHOUT a live source
        // it seeds the rest pose only ONCE, so the bones written last frame still
        // hold OUR OWN output and the sim chases itself and drifts. Restore rest for
        // exactly the bones the solver writes; a non-solo strand root and the anchors
        // above it stay with animation / head-track. Serial, before the FK cache and
        // Simulate read it.
        // The test is AnimationSystem::HasLivePoseSource, not primaryClip alone: a
        // LocomotionComponent blend space or an AnimLayer is a live source too, and
        // resetting those characters would flatten the chain bones to bind pose
        // every frame. An A/B shadow takes its SOURCE's verdict (its input pose is
        // the source's, re-established exactly when the source's is).
        const bool clipActive = m_animSys.HasLivePoseSource(world, src);
        if (!clipActive)
        {
            auto resetRot = [&](uint32_t bn)
            {
                if (bn >= skel.boneCount) return;
                // The authored rotation IS this bone's clean base: it was
                // stamped above. Restoring skel.restPoseLocal here would delete
                // it every frame on a character with no live pose source (the
                // tail holds its pose in Edit mode and snaps straight the
                // instant you press Play, while any character with a clip
                // behaves).
                if (ChainPhysicsSystem::IsRestPosedBone(*phys, bn)) return;
                XMVECTOR sc, rq, tr;
                if (XMMatrixDecompose(&sc, &rq, &tr, XMLoadFloat4x4(&skel.restPoseLocal[bn])))
                    XMStoreFloat4(&poses[bn].rot, XMQuaternionNormalize(rq));
            };
            // Strand jiggle bones: the same range WriteBonePoses writes (rootSolo
            // strands include their root bone i=0; the composed root swing would
            // otherwise stack onto last frame's output and spin up, since without
            // a clip nothing re-establishes the rest rotation).
            for (const auto& strand : data.strands)
            {
                const uint32_t iBegin = strand.rootSolo ? 0u : 1u;
                for (uint32_t i = iBegin; i + 1 < strand.particleCount; ++i)
                    resetRot(data.particles[strand.particleOffset + i].boneIndex);
            }
            // Spring jiggle bones: the same guard WriteSpringBonePoses uses.
            for (const auto& sb : data.springBones)
            {
                if (sb.isVirtual || sb.boneIndex >= skel.boneCount) continue;
                if (skel.parentIndex[sb.boneIndex] < 0) continue;
                resetRot(sb.boneIndex);
            }
        }

        // Close the SERIAL pass here: everything above either mutates shared
        // state (m_entityData insertion in InitEntity) or writes `poses`, which
        // the FK compose reads, so it cannot move. The compose itself is
        // per-entity (this entity's poses and the skeleton's read-only
        // parentIndex in, this entity's boneWorldMatCache out), so it runs in
        // parallel after this loop (composeEntity): it was ~80% of the serial
        // phase's cost.
        data.boneWorldMatCache.resize(skel.boneCount);

        PhysJob job;
        job.e     = e;
        job.src   = src;   // a shadow reads its source's World data
        job.phys  = phys;
        job.poses = poses;
        job.skel  = &skel;
        job.data  = &data;
        job.capsuleCount = 0;
        // Preview-lane jobs carry the preview scene.
        if (lane == Lane::Preview && (m_previewGravityActive || m_previewSceneCapCount != 0))
        {
            // World -> this entity's skeleton space (serial phase-1 read of
            // the entity's GlobalTransform; no transform = identity).
            XMMATRIX W = XMMatrixIdentity();
            if (const auto* gt = world.GetComponent<GlobalTransform>(src))   // M7: a shadow maps through its source
                W = XMLoadFloat4x4(&gt->matrix);
            XMVECTOR det;
            const XMMATRIX inv = XMMatrixInverse(&det, W);
            const bool invOk = std::fabs(XMVectorGetX(det)) > 1e-20f;
            if (m_previewGravityActive)
            {
                // The DIRECTION moves into skeleton space, the magnitude stays
                // |down| (the authored gravity is already in skeleton units).
                const XMVECTOR d   = XMLoadFloat3(&m_previewGravityDown);
                const float    len = XMVectorGetX(XMVector3Length(d));
                XMVECTOR ds = invOk ? XMVector3TransformNormal(d, inv) : d;
                const float lds = XMVectorGetX(XMVector3Length(ds));
                ds = (lds > 1e-12f) ? XMVectorScale(ds, len / lds) : XMVectorZero();
                XMStoreFloat3(&job.gravitySkel, ds);
                job.gravityOvr = true;
            }
            if (m_previewSceneCapCount != 0 && invOk)
            {
                job.sceneCaps = true;
                XMStoreFloat4x4(&job.invWorld, inv);
                // uniform scale estimate: the mean row length of the inverse
                job.invScale = (XMVectorGetX(XMVector3Length(inv.r[0])) +
                                XMVectorGetX(XMVector3Length(inv.r[1])) +
                                XMVectorGetX(XMVector3Length(inv.r[2]))) / 3.f;
            }
        }
        // ---- Shadow-only half rate (LodTiers::shadowOnlyHalfRate) ----
        // Serial, before the world kick (a held frame must not consume the
        // entity-motion bookkeeping: the next solving pass sees both frames'
        // motion, which is what its doubled dt integrates). Never on the frame
        // an entity was (re)initialised, never twice in a row, never on the
        // preview lane, never with world-motion inertia authored, and only
        // while 2*dt fits under Simulate's 1/30 clamp.
        job.dt = dt;
        if (m_hold && !m_simulationEnabled && lane == Lane::Main)
            job.hold = true;   // time stands still (SetHold): every main-lane entity holds
        else
        {
            static const bool s_rateCli = Sys::CommandLine::Get().Has("-chainlodshadowrate");
            const bool rateOn = lodEnabled && (lodTiers.shadowOnlyHalfRate || s_rateCli);
            if (rateOn && lane == Lane::Main && !justInitialized && data.rateCarry == 0.f &&
                2.f * dt <= (1.f / 30.f) + 1e-6f &&   // Simulate's clamp
                m_shadowOnly && m_shadowOnly->count(e) &&
                // world-motion inertia counts passes (EntityData::wmPass); an
                // entity that authors it always solves
                !(!m_worldInertiaSuppressed && WantsWorldInertia(*phys)) &&
                ((m_updateSerial + static_cast<uint64_t>(e)) & 1u) != 0u)
            {
                job.hold = true;
                data.rateCarry = dt;
                ++m_rateHolds;
            }
            else if (data.rateCarry > 0.f)
            {
                job.dt = dt + data.rateCarry;
                data.rateCarry = 0.f;
            }
        }
        // World-motion inertia: the entity motion since this entity's previous
        // pass of THIS lane (src's GlobalTransform: an A/B shadow moves with its
        // source). Serial: it reads the World and writes this entity's
        // bookkeeping; the island workers only read job.worldKick. A component
        // that authors no world inertia reads and writes nothing here.
        if (!job.hold && !m_worldInertiaSuppressed && WantsWorldInertia(*phys))
            job.worldKickOn = PrepareWorldKick(world, e, src, data, *phys, job.dt, lane, job.worldKick);
        jobs.push_back(job);
    }

    // Lane bookkeeping, serial. solvedBothLanes counts the entities THIS lane
    // pushed into jobs that the other lane ALSO pushed in the same Update,
    // measured on the job lists, not as a set intersection: the pin set puts
    // the stage into the main lane's activeSet by design, so
    // |activeSet AND exclude| == 1 is expected and says nothing. Armed only
    // while a preview set is installed (one pointer test for the main lane
    // otherwise); RunPass called directly (the unit test) counts as one Update.
    if (exclude) m_laneStats.excludedByPreviewLane += excludedHere;
    if (lane == Lane::Preview || m_previewSet)
    {
        const int self = static_cast<int>(lane), other = 1 - self;
        std::vector<Entity>& mine = m_laneSolved[self];
        mine.clear();
        for (const auto& j : jobs) mine.push_back(j.e);
        std::sort(mine.begin(), mine.end());
        m_laneSolvedSerial[self] = m_updateSerial;
        if (m_laneSolvedSerial[other] == m_updateSerial)
            for (Entity se : mine)
                if (std::binary_search(m_laneSolved[other].begin(),
                                       m_laneSolved[other].end(), se))
                    ++m_laneStats.solvedBothLanes;
    }

    // Snapshot World lookups on the owner before dispatch. No pool/table
    // growth, camera mutation or component edits are allowed until join.
    for (auto& job : jobs) {
        // job.src: an A/B shadow collides with (and is placed by) its
        // source's capsules / transform. == job.e for everything else.
        const Entity e = job.src;
        const auto* capComp = world.GetComponent<CapsuleColliderComponent>(e);
        if (!capComp)
        {
            const Children* ch = world.GetComponent<Children>(e);
            if (ch)
                for (Entity child : ch->entities)
                {
                    capComp = world.GetComponent<CapsuleColliderComponent>(child);
                    if (capComp && capComp->count > 0) break;
                    capComp = nullptr;
                }
        }

        job.capsuleSource = capComp;
        if (const auto* gt = world.GetComponent<GlobalTransform>(e))
            job.worldPosition = {gt->matrix._41, gt->matrix._42, gt->matrix._43};
        job.shadowOnly = m_shadowOnly && m_shadowOnly->count(e);
    }
    auto composeEntity = [&](uint32_t ji) {
        ZoneScopedN("Chain.Prepare.FKEntity");
        PhysJob&   j     = jobs[ji];
        const auto& skel = *j.skel;
        const auto* poses = j.poses;
        auto&       cache = j.data->boneWorldMatCache;
        for (uint32_t b = 0; b < skel.boneCount; ++b)
        {
            const auto& lp = poses[b];
            XMMATRIX local = XMMatrixScaling(lp.scl.x, lp.scl.y, lp.scl.z)
                           * XMMatrixRotationQuaternion(XMLoadFloat4(&lp.rot))
                           * XMMatrixTranslation(lp.pos.x, lp.pos.y, lp.pos.z);
            const int32_t parent = skel.parentIndex[b];
            if (parent >= 0)
                local = local * XMLoadFloat4x4(&cache[parent]);
            XMStoreFloat4x4(&cache[b], local);
        }
    };
    auto prepareEntity = [&](uint32_t ji) {
        ZoneScopedN("Chain.Prepare.Entity");
        auto& job = jobs[ji];
        ChainPhysicsComponent*      phys  = job.phys;
        const SkeletonAsset&        skel  = *job.skel;
        EntityData&                 data  = *job.data;

        // The capsule source was looked up on the owner thread before dispatch;
        // this entity exclusively owns the job's output.
        const auto* capComp = job.capsuleSource;

        if (capComp && capComp->count > 0)
        {
            for (int ci = 0; ci < capComp->count; ++ci)
            {
                const auto& def = capComp->capsules[ci];
                if (!def.enabled) continue;
                if (def.boneA >= skel.boneCount || def.boneB >= skel.boneCount) continue;

                XMMATRIX wA = XMLoadFloat4x4(&data.boneWorldMatCache[def.boneA]);
                XMMATRIX wB = XMLoadFloat4x4(&data.boneWorldMatCache[def.boneB]);
                XMVECTOR posA = XMVectorAdd(wA.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetA), wA));
                XMVECTOR posB = XMVectorAdd(wB.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetB), wB));

                XMStoreFloat3(&job.capsules[job.capsuleCount].a, posA);
                XMStoreFloat3(&job.capsules[job.capsuleCount].b, posB);
                job.capsules[job.capsuleCount].radius = def.radius;
                job.capsuleCount++;
            }
        }
        // The preview scene's world-space capsules (the floor proxy), after
        // the bone-attached ones and never past the job's fixed array.
        // Preview-lane jobs only (sceneCaps is false otherwise); the members
        // are frame-constant here (written in the serial window).
        if (job.sceneCaps)
            for (std::uint32_t si = 0; si < m_previewSceneCapCount &&
                 job.capsuleCount < CapsuleColliderComponent::MAX_CAPSULES; ++si)
            {
                // world -> this entity's skeleton space
                const XMMATRIX inv = XMLoadFloat4x4(&job.invWorld);
                const WorldCapsule& wc = m_previewSceneCaps[si];
                WorldCapsule& c = job.capsules[job.capsuleCount++];
                XMStoreFloat3(&c.a, XMVector3TransformCoord(XMLoadFloat3(&wc.a), inv));
                XMStoreFloat3(&c.b, XMVector3TransformCoord(XMLoadFloat3(&wc.b), inv));
                c.radius = wc.radius * job.invScale;
                ++job.sceneCapCount;
            }

        // Resolve the per-group stiffness tables once for this entity, before any
        // of its island tasks run: they all read them concurrently afterwards.
        {
            const auto& gcfg = *phys;
            for (int gi = 0; gi < 256; ++gi)
            {
                const ChainGroupDef* o =
                    (static_cast<size_t>(gi) < gcfg.groups.size() && gcfg.groups[gi].ovrEnabled)
                        ? &gcfg.groups[gi] : nullptr;
                data.groupTables.vert[gi]       = o ? o->ovrStructuralStiffness : gcfg.structuralStiffness;
                data.groupTables.ring[gi]       = o ? o->ovrRingStiffness       : gcfg.ringStiffness;
                data.groupTables.shear[gi]      = o ? o->ovrShearStiffness      : gcfg.shearStiffness;
                data.groupTables.horizDecay[gi] = o ? o->ovrHorizDecay          : gcfg.skirtHorizDecay;
            }
        }
        // ---- Distance LOD ----
        // Resolved in the entity's preparation, next to the substep decision, so
        // both land on the job together and the island solve reads neither the
        // camera nor the World.
        int  lodSubCap    = 8;
        int  lodIterations = phys->iterations;
        bool lodCollide    = true;
        if (lodEnabled)
        {
            // GlobalTransform, NOT boneWorldMatCache[0]: the bone cache is in
            // SKELETON space (a character authored ~18.8 units tall may be
            // instanced at scale 0.1), which would put every character at the
            // same small "distance" and in tier 0. Distance LOD needs world
            // space.
            const XMFLOAT3 bp = job.worldPosition;
            const float ddx = bp.x - m_cameraPos.x;
            const float ddy = bp.y - m_cameraPos.y;
            const float ddz = bp.z - m_cameraPos.z;
            const float dist = std::sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);

            // Hysteresis: widen the band the character is ALREADY in, so one
            // parked on a threshold does not chatter between resting on a leg
            // and sinking into it.
            const float h  = lodTiers.hysteresis;
            const int   was = data.lodTier;
            float t1 = lodTiers.tier1Distance;
            float t2 = lodTiers.tier2Distance;
            if (was >= 1) t1 -= h;
            if (was >= 2) t2 -= h;

            job.lodDistance = dist;

            int tier = 0;
            if      (dist >= t2) tier = 2;
            else if (dist >= t1) tier = 1;

            // Off screen and alive only for a shadow cascade: demote one tier.
            // Distance alone misses this: a character right behind the camera
            // is CLOSE and invisible, yet would pay full price to cast a
            // silhouette.
            if (job.shadowOnly)
                tier = std::min(2, tier + 1);

            data.lodTier = tier;

            switch (tier)
            {
            case 2:
                // The whole win: hasCapsules gates EVERY SolveCapsuleCollision
                // call, and collision is ~80% of the solver. The authored limit
                // cone still bounds the silhouette.
                lodCollide    = false;
                lodIterations = 1;
                lodSubCap     = 1;
                break;
            case 1:
                lodIterations = std::max(1, phys->iterations / 2);
                lodSubCap     = 2;
                break;
            default:
                break;
            }

            // The shadow-only profile (LodTiers::shadowOnlyIterations): fewer
            // iterations and substeps, collision kept - see the field. Only a
            // tier-1 shadow-only entity is affected (tier 2 is already cheaper).
            static const int s_shadowItCli =
                Sys::CommandLine::Get().GetInt("-chainlodshadow", -1);
            const int shadowIt = s_shadowItCli >= 0 ? s_shadowItCli : lodTiers.shadowOnlyIterations;
            if (job.shadowOnly && tier < 2 && shadowIt > 0)
            {
                lodIterations = std::min(lodIterations, shadowIt);
                lodSubCap     = std::min(lodSubCap, std::max(1, lodTiers.shadowOnlySubCap));
            }
        }
        else
        {
            data.lodTier = 0;
        }
        job.iterations = std::max(1, lodIterations);
        job.collide    = lodCollide;
        job.tier       = lodEnabled ? data.lodTier : 0;

        job.nSub = PrepareSubsteps(data, *phys, job.capsules, job.capsuleCount, job.dt, lodSubCap);
    };
    // A held job's whole pass: the bones re-written from the particles as they
    // are, island by island in Simulate's order (HoldPass's inner loop). Per
    // entity, touches only this entity's data and pose: safe beside solves.
    auto holdEntity = [&](uint32_t ji) {
        ZoneScopedN("Chain.Hold");
        PhysJob& j = jobs[ji];
        for (const Island& isl : j.data->islands)
        {
            if (isl.strandEnd > isl.strandBegin)
                WriteBonePoses(*j.data, isl, j.poses, *j.skel);
            if (isl.springs && !j.data->springBones.empty())
                WriteSpringBonePoses(*j.data, j.poses, *j.skel);
        }
    };
    auto finishPreparation = [&] {
    // Owner-only logging and profile reduction after all entity writes finish.
    if (lodEnabled && !jobs.empty()) {
        const auto bp = jobs.front().worldPosition;
        const float dist = jobs.front().lodDistance;
        // One-shot: a camera at the origin, or in the wrong space, presents
        // as "every character is tier 0", indistinguishable from the LOD
        // being off. Print the first classification so it cannot.
        static bool s_loggedLod = false;
        if (!s_loggedLod)
        {
            s_loggedLod = true;
            LOG_INFO("ChainPhysics LOD: camera=(%.1f,%.1f,%.1f) first entity at "
                     "(%.1f,%.1f,%.1f) dist=%.1f (tier1 %.1f, tier2 %.1f)",
                     m_cameraPos.x, m_cameraPos.y, m_cameraPos.z,
                     bp.x, bp.y, bp.z, dist,
                     lodTiers.tier1Distance, lodTiers.tier2Distance);
        }

    }
    for (const auto& job : jobs) {
        // A held job ran no solve: it counts as an entity, never as a
        // tier or as substeps (its tier / nSub were not resolved this pass).
        if (job.hold) { Anim::AnimProfile::Get().chainEntities += 1u; continue; }
        // Aggregate substep load. The adaptive path raises nSub from how far a
        // collider capsule moved SINCE LAST FRAME, and that distance scales
        // with dt: a slow frame buys more substeps, which cost more, which
        // makes the next frame slower. Without this counter the ceiling is
        // invisible; the only symptom is that chain physics is expensive.
        {
            auto& prof = Anim::AnimProfile::Get();
            prof.chainEntities  += 1u;
            prof.chainSubsteps += static_cast<std::uint32_t>(job.nSub);
            // Tier histogram. Not optional: a per-frame quality rewrite silently
            // invalidates every measurement taken over it (AILODSystem carries the
            // same warning), so a bench run has to be able to see the
            // distribution and pin it off.
            if (job.tier == 0)      prof.chainTier0 += 1u;
            else if (job.tier == 1) prof.chainTier1 += 1u;
            else                    prof.chainTier2 += 1u;
            if (lane == Lane::Main && job.shadowOnly) prof.chainShadowOnly += 1u;
            // The PREVIEW lane's own share of the five accumulators above, so
            // the panel's stats overlay shows the stage's numbers. The fields
            // above still sum both lanes, as every existing reader (AIStats,
            // perf packs) expects; these are extra and preview-only.
            if (lane == Lane::Preview)
            {
                prof.previewChainEntities += 1u;
                prof.previewChainSubsteps += static_cast<std::uint32_t>(job.nSub);
                if (job.tier == 0)      prof.previewChainTier0 += 1u;
                else if (job.tier == 1) prof.previewChainTier1 += 1u;
                else                    prof.previewChainTier2 += 1u;
            }
        }

    }
    // The preview lane's own load for the "[CHAINPHYS] preview lane:"
    // line (chainEntities / chainSubsteps above ACCUMULATE across both
    // lanes by design).
    if (lane == Lane::Preview)
    {
        std::uint32_t subs = 0;
        for (const auto& job : jobs)
        {
            subs += static_cast<std::uint32_t>(job.nSub);
            // What LOD the preview lane really ran with. Read-only on the
            // job list; nothing in the solve reads these two counters.
            m_laneStats.maxTier = std::max(m_laneStats.maxTier,
                                           static_cast<std::uint32_t>(job.tier));
            m_laneStats.minIterations = std::min(m_laneStats.minIterations,
                                                 static_cast<std::uint32_t>(job.iterations));
        }
        m_laneStats.entities = static_cast<std::uint32_t>(jobs.size());
        m_laneStats.substeps = subs;
        // The preview scene this pass actually carried.
        std::uint32_t sceneCaps = 0, gravOvr = 0;
        for (const auto& job : jobs)
        {
            sceneCaps += static_cast<std::uint32_t>(job.sceneCapCount);
            gravOvr    = gravOvr | (job.gravityOvr ? 1u : 0u);
        }
        m_laneStats.sceneCapsules   = sceneCaps;
        m_laneStats.gravityOverride = gravOvr;
    }

    };
    // Keep cross-island parallelism for small populations; stream larger ones.
    static const bool s_disablePipeline = Sys::CommandLine::Get().Has("-chainpipelineoff");
    const bool requestPipeline = !s_disablePipeline;
#if WITH_HARNESS
    static const bool s_verifyPipeline = Sys::CommandLine::Get().Has("-chainpipelineverify");
#endif
    const bool usePipeline = requestPipeline && jobs.size() >= 2u * TaskSystem::Get().GetWorkerCount();
    ZoneNamedN(pipelineMode, "Chain.Execute.Pipeline", usePipeline);
    ZoneNamedN(legacyMode, "Chain.Execute.Legacy", !usePipeline);
    if (usePipeline) {
        ZoneScopedN("Chain.Pipeline");
        const auto pipelineStart = std::chrono::steady_clock::now();
        const auto count = static_cast<uint32_t>(jobs.size());
        std::atomic<long long> solverNs{0};
        // Per-tier / shadow-only / preparation CPU time (AnimProfile chainTierCpuMs,
        // chainShadowOnlyCpuMs, chainPrepCpuMs).
        std::atomic<long long> tierNs[3]{}, shadowOnlyNs{0}, prepNs{0};
        std::atomic<uint32_t> prepared{0}, firstSolverPrepared{~0u};
        auto solveEntity = [&](uint32_t i) {
            ZoneScopedN("Chain.Pipeline.SolveEntity");
            PROF_SCOPE("Chain.Solve");
            auto& j = jobs[i];
            const auto start = std::chrono::steady_clock::now();
            uint32_t unset = ~0u;
            firstSolverPrepared.compare_exchange_strong(unset, prepared.load());
            for (const auto& task : j.data->islandTasks)
                for (int ii : task)
                    if (Simulate(*j.data, j.data->islands[ii], *j.phys, j.dt, j.nSub,
                                 j.iterations, j.collide, j.poses, *j.skel, j.capsules, j.capsuleCount, swayTime,
                                 j.gravityOvr ? &j.gravitySkel : nullptr,
                                 j.worldKickOn ? &j.worldKick : nullptr))
                        j.clamped = true;
            const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            solverNs.fetch_add(ns, std::memory_order_relaxed);
            tierNs[j.tier >= 0 && j.tier < 3 ? j.tier : 2].fetch_add(ns, std::memory_order_relaxed);
            if (j.shadowOnly) shadowOnlyNs.fetch_add(ns, std::memory_order_relaxed);
        };
        std::vector<uint32_t> order(count);
        std::atomic<uint32_t> nextEntity{0};
        auto pipeline = [&]() -> Sys::CpuTask {
            for (uint32_t index = nextEntity.fetch_add(1); index < count; index = nextEntity.fetch_add(1)) {
                const uint32_t i = order[index];
                if (jobs[i].hold)   // held: no prepare, no solve
                {
                    co_await Sys::SubmitCpu([&, i] { PROF_SCOPE("Chain.Hold"); holdEntity(i); });
                    continue;
                }
                co_await Sys::SubmitCpu([&, i] {
                    PROF_SCOPE("Chain.Prepare");
                    const auto t0 = std::chrono::steady_clock::now();
                    composeEntity(i);
                    prepareEntity(i);
                    prepNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
                    prepared.fetch_add(1);
                });
                co_await Sys::SubmitCpu([&, i] { solveEntity(i); });
            }
        };
        auto runPipeline = [&] {
            for (uint32_t i = 0; i < count; ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                const auto cost = [&](uint32_t i) { return jobs[i].data->particles.size() + jobs[i].data->springBones.size(); };
                return cost(a) > cost(b);
            });
            // The coroutine closure, jobs and all borrowed inputs outlive Join.
            // Awaiting dependencies suspends the coroutine, never the worker.
            // Bounded in-flight entities: enqueueing every preparation first
            // would rebuild the global barrier via FIFO queue order. Lanes
            // advance to another entity only after their current solver ends.
            const uint32_t lanes = (std::min)(count, TaskSystem::Get().GetWorkerCount());
            Sys::CpuTaskScope scope(lanes);
            for (uint32_t lane = 0; lane < lanes; ++lane) scope.Start(pipeline());
            scope.Join();
        };
        static uint64_t s_pipelineFrame = 0;
        ++s_pipelineFrame;
#if WITH_HARNESS
        // The reference below does not model held jobs: skip a frame that has one.
        const bool anyHold = std::any_of(jobs.begin(), jobs.end(), [](const PhysJob& j) { return j.hold; });
        if (s_verifyPipeline && !anyHold && (s_pipelineFrame <= 3 || s_pipelineFrame % 60 == 0)) {
            std::vector<EntityData> original, expected;
            std::vector<std::vector<AnimationSystem::LocalPose>> poses, expectedPoses;
            for (const auto& j : jobs) {
                original.push_back(*j.data);
                poses.emplace_back(j.poses, j.poses + j.skel->boneCount);
            }
            const auto initialJobs = jobs;
            // The reference is the non-pipelined path: an all-entity FK barrier,
            // serial prepare, cost-sorted interleaved island dispatch and per-task
            // clamp flags.
            TaskSystem::Get().ParallelFor(0, count, composeEntity);
            for (uint32_t i = 0; i < count; ++i) prepareEntity(i);
            struct RefTask { uint32_t entity, task; size_t cost; bool clamped = false; };
            std::vector<RefTask> refs;
            for (uint32_t i = 0; i < count; ++i)
                for (uint32_t t = 0; t < jobs[i].data->islandTasks.size(); ++t) {
                    size_t cost = 0;
                    for (int ii : jobs[i].data->islandTasks[t]) {
                        const auto& isl = jobs[i].data->islands[ii];
                        cost += isl.particleEnd - isl.particleBegin;
                        if (isl.springs) cost += jobs[i].data->springBones.size();
                    }
                    refs.push_back({i, t, cost});
                }
            std::stable_sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) { return a.cost > b.cost; });
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(refs.size()), [&](uint32_t r) {
                auto& ref = refs[r]; auto& j = jobs[ref.entity];
                for (int ii : j.data->islandTasks[ref.task])
                    if (Simulate(*j.data, j.data->islands[ii], *j.phys, dt, j.nSub,
                                 j.iterations, j.collide, j.poses, *j.skel, j.capsules, j.capsuleCount, swayTime,
                                 j.gravityOvr ? &j.gravitySkel : nullptr,
                                 j.worldKickOn ? &j.worldKick : nullptr))
                        ref.clamped = true;
            }, TaskSystem::TaskPriority::High, TaskSystem::ParallelSchedule::Interleaved);
            for (const auto& r : refs) jobs[r.entity].clamped |= r.clamped;
            const auto expectedJobs = jobs;
            for (const auto& j : jobs) {
                expected.push_back(*j.data);
                expectedPoses.emplace_back(j.poses, j.poses + j.skel->boneCount);
            }
            for (uint32_t i = 0; i < count; ++i) {
                *jobs[i].data = original[i];
                std::copy(poses[i].begin(), poses[i].end(), jobs[i].poses);
            }
            jobs = initialJobs;
            runPipeline();
            bool match = true;
            auto equal = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
            for (uint32_t i = 0; i < count; ++i) {
                const auto& j = jobs[i]; const auto& k = expectedJobs[i];
                const auto& a = *j.data; const auto& b = expected[i];
                match = match && j.clamped == k.clamped && j.nSub == k.nSub && j.tier == k.tier &&
                    j.iterations == k.iterations && j.collide == k.collide && j.capsuleCount == k.capsuleCount &&
                    a.velocityStepSeconds == b.velocityStepSeconds && a.lodTier == b.lodTier && equal(a.groupTables, b.groupTables);
                for (int c = 0; c < j.capsuleCount; ++c) match = match && equal(j.capsules[c], k.capsules[c]);
                for (uint32_t n = 0; n < j.skel->boneCount; ++n)
                    match = match && equal(j.poses[n], expectedPoses[i][n]) && equal(a.boneWorldMatCache[n], b.boneWorldMatCache[n]);
                for (size_t n = 0; n < a.particles.size(); ++n) match = match && equal(a.particles[n], b.particles[n]);
                for (size_t n = 0; n < a.springBones.size(); ++n) {
                    const auto& x = a.springBones[n]; const auto& y = b.springBones[n];
                    match = match && equal(x.currentWorldPos, y.currentWorldPos) && equal(x.velocity, y.velocity) && equal(x.animMidpoint, y.animMidpoint);
                }
            }
            ENGINE_CHECK(match, "chain pipeline oracle mismatch");
            LOG_INFO("[CHAIN_PIPELINE_ORACLE] entities=%u match=%u", count, match ? 1u : 0u);
        }
        else
#endif
            runPipeline();
        finishPreparation();
        auto& prof = Anim::AnimProfile::Get();
        // These three are ASSIGNED, not accumulated, and TickAnimationChain
        // never clears them, so a second pass per frame would overwrite the
        // scene's numbers: Lane::Main writes chain*Ms, the preview lane writes
        // previewChain*Ms.
        (lane == Lane::Main ? prof.chainSerialMs : prof.previewChainSerialMs) = static_cast<float>(std::chrono::duration<double, std::milli>(pipelineStart - _chainT0).count());
        // On this path the parallel latency includes FK + prepare + solve (the
        // non-pipelined path times the island solve only).
        (lane == Lane::Main ? prof.chainParallelMs : prof.previewChainParallelMs) = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipelineStart).count());
        (lane == Lane::Main ? prof.chainCpuMs : prof.previewChainCpuMs) = static_cast<float>(solverNs.load() / 1e6);
        if (lane == Lane::Main)
        {
            for (int t = 0; t < 3; ++t) prof.chainTierCpuMs[t] = static_cast<float>(tierNs[t].load() / 1e6);
            prof.chainShadowOnlyCpuMs = static_cast<float>(shadowOnlyNs.load() / 1e6);
            prof.chainPrepCpuMs       = static_cast<float>(prepNs.load() / 1e6);
        }
        for (const auto& j : jobs) {
            if (j.clamped && !j.data->divergenceLogged) {
                j.data->divergenceLogged = true;
                LOG_WARNING("ChainPhysics: clamped a diverging chain (pipeline)");
            }
            if (j.hold) continue;   // held: keeps the capsules of its last solve
            if (j.capsuleCount) j.data->prevCapsules.assign(j.capsules, j.capsules + j.capsuleCount);
            else j.data->prevCapsules.clear();
            // FillDebugSnapshot's own / scene capsule split (nothing in the solve reads it).
            j.data->prevSceneCaps = static_cast<std::uint32_t>(j.sceneCapCount);
        }
        if (s_pipelineFrame % 60 == 0)
            LOG_INFO("[CHAIN_PIPELINE] entities=%u firstSolverPrepared=%u", count, firstSolverPrepared.load());
        return;
    }

    bool graphPrep = false, verifyPrep = false;
#if WITH_HARNESS
    static const bool s_requestGraph = Sys::CommandLine::Get().Has("-chainprepgraph");
    static const bool s_requestVerify = Sys::CommandLine::Get().Has("-chainprepverify");
    static uint64_t s_prepEpoch = 0;
    ++s_prepEpoch;
    graphPrep = s_requestGraph;
    verifyPrep = s_requestVerify && (s_prepEpoch <= 3 || s_prepEpoch % 60 == 0);
#endif
    auto runPreparation = [&](bool graph) {
        ZoneNamedN(graphZone, "Chain.Prepare.Graph", graph);
        ZoneNamedN(legacyZone, "Chain.Prepare.Legacy", !graph);
        const uint32_t count = static_cast<uint32_t>(jobs.size());
        if (!graph) {
            if (count) TaskSystem::Get().ParallelFor(0, count, composeEntity);
            for (uint32_t i = 0; i < count; ++i) if (!jobs[i].hold) prepareEntity(i);   // held jobs skip preparation
            return;
        }
        // Coarse entity chunks; FK and dependent preparation stay together.
        // This removes the all-entities FK barrier before preparation, without
        // nesting worker waits or changing the subsequent island solver.
        const uint32_t lanes = (std::max)(1u, (std::min)(
            TaskSystem::Get().GetWorkerCount(), (count + 15u) / 16u));
        auto run = [&](uint32_t lane) {
            ZoneScopedN("Chain.Prepare.Lane");
            const uint32_t lo = count * lane / lanes, hi = count * (lane + 1) / lanes;
            for (uint32_t i = lo; i < hi; ++i) { composeEntity(i); if (!jobs[i].hold) prepareEntity(i); }
        };
        Sys::CpuTaskScope scope(lanes - 1);
        for (uint32_t lane = 1; lane < lanes; ++lane) scope.Submit([&, lane] { run(lane); });
        run(0);
        scope.Join();
    };
#if WITH_HARNESS
    if (verifyPrep && graphPrep) {
        // Expensive same-start-state oracle: validate every output consumed by
        // the island solver, then continue with the graph result. No sim rerun.
        std::vector<EntityData> original, expected;
        for (const auto& j : jobs) original.push_back(*j.data);
        const auto initialJobs = jobs;
        runPreparation(false);
        for (const auto& j : jobs) expected.push_back(*j.data);
        const auto expectedJobs = jobs;
        for (size_t i = 0; i < jobs.size(); ++i) *jobs[i].data = original[i];
        jobs = initialJobs;
        runPreparation(true);
        bool match = true;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto& a = *jobs[i].data; const auto& b = expected[i];
            const auto& j = jobs[i]; const auto& k = expectedJobs[i];
            match = match && j.capsuleCount == k.capsuleCount && j.nSub == k.nSub &&
                j.iterations == k.iterations && j.collide == k.collide && j.tier == k.tier &&
                a.lodTier == b.lodTier && a.velocityStepSeconds == b.velocityStepSeconds &&
                a.boneWorldMatCache.size() == b.boneWorldMatCache.size() &&
                std::memcmp(&a.groupTables, &b.groupTables, sizeof(a.groupTables)) == 0;
            if (!a.boneWorldMatCache.empty()) match = match && std::memcmp(a.boneWorldMatCache.data(),
                b.boneWorldMatCache.data(), a.boneWorldMatCache.size() * sizeof(XMFLOAT4X4)) == 0;
            for (int c = 0; c < j.capsuleCount; ++c)
                match = match && std::memcmp(&j.capsules[c], &k.capsules[c], sizeof(WorldCapsule)) == 0;
            for (size_t q = 0; q < a.particles.size(); ++q)
                match = match && std::memcmp(&a.particles[q].prevPosition, &b.particles[q].prevPosition, sizeof(XMFLOAT3)) == 0;
        }
        ENGINE_CHECK(match, "chain preparation graph oracle mismatch");
        LOG_INFO("[CHAIN_PREP_ORACLE] entities=%zu match=%u", jobs.size(), match ? 1u : 0u);
    }
    else
#endif
        runPreparation(graphPrep);

    finishPreparation();

    // Phase 2 (parallel): ONE flat ParallelFor over (entity x island-task)
    // pairs, so one character's cloth pieces / hair chains also spread
    // across workers. NEVER nest another ParallelFor inside this one:
    // TaskSystem's caller blocks without work-stealing, so nesting past
    // depth 1 can strand queued chunks (a hang on low core counts).
    struct IslandJob {
        const PhysJob*          job;
        const std::vector<int>* islandIdxs;   // one BuildIslands task (usually 1 island)
        // Sort key, computed ONCE here instead of inside the comparator:
        // stable_sort makes ~n log2(n) comparisons, and each cost evaluation is
        // a chain of dependent loads (jobs[] -> EntityData* -> the islands heap
        // -> the islandTasks inner vector).
        // Safe because the key is frame-invariant: islands / islandTasks are
        // filled only by BuildIslands (called from InitEntity, behind
        // !initialized) and never cleared anywhere else, so the order is the
        // same as with the cost evaluated inside the comparator.
        size_t                  cost = 0;
        bool                    clamped = false;
    };
    std::vector<IslandJob> islandJobs;
    {
        size_t nTasks = 0;
        for (const auto& j : jobs) nTasks += j.data->islandTasks.size();
        islandJobs.reserve(nTasks);
    }
    for (const auto& j : jobs)
        if (!j.hold)   // a held job has no island to solve
        for (const auto& task : j.data->islandTasks)
        {
            size_t cost = 0;
            for (int ii : task)
            {
                const auto& isl = j.data->islands[ii];
                cost += isl.particleEnd - isl.particleBegin;
                if (isl.springs) cost += j.data->springBones.size();
            }
            islandJobs.push_back({ &j, &task, cost });
        }

    // Priciest tasks first. With tasks <= workers each task is its own chunk
    // and the big cloth rings start immediately instead of queueing behind hair
    // strands. Past that point the order alone would CONCENTRATE the expensive
    // tasks in one chunk, so the dispatch below asks for an interleaved deal:
    // the two go together, and changing either without the other brings back
    // the main-thread pile-up described at the ParallelFor call.
    std::stable_sort(islandJobs.begin(), islandJobs.end(),
        [](const IslandJob& a, const IslandJob& b) { return a.cost > b.cost; });

    auto runTask = [&](IslandJob& ij)
    {
        const PhysJob& j = *ij.job;
        for (int ii : *ij.islandIdxs)
            if (Simulate(*j.data, j.data->islands[ii], *j.phys, j.dt, j.nSub,
                         j.iterations, j.collide,
                         j.poses, *j.skel, j.capsules, j.capsuleCount, swayTime,
                         j.gravityOvr ? &j.gravitySkel : nullptr,
                                 j.worldKickOn ? &j.worldKick : nullptr))
                ij.clamped = true;
    };

    // ---- Phase 1 ends here ----
    // chainSerialMs is phase latency (including preparation jobs and owner
    // joins), not serial CPU time; with chainParallelMs it splits the chain
    // cost into its two halves. Outside the if, so the single-job path is
    // measured too.
    {
        auto& _ap = Anim::AnimProfile::Get();
        // Per lane: see the pipeline path above.
        (lane == Lane::Main ? _ap.chainSerialMs : _ap.previewChainSerialMs) = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - _chainT0).count());
    }
    const auto _chainPar0 = std::chrono::steady_clock::now();
    std::atomic<long long> _chainCpuNs{ 0 };
    std::atomic<long long> _tierNs[3]{}, _shadowOnlyNs{ 0 };
    auto _timedTask = [&](uint32_t i)
    {
        PROF_SCOPE("Chain.Solve");
        const auto t0 = std::chrono::steady_clock::now();
        runTask(islandJobs[i]);
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        _chainCpuNs.fetch_add(ns, std::memory_order_relaxed);
        const PhysJob& j = *islandJobs[i].job;
        _tierNs[j.tier >= 0 && j.tier < 3 ? j.tier : 2].fetch_add(ns, std::memory_order_relaxed);
        if (j.shadowOnly) _shadowOnlyNs.fetch_add(ns, std::memory_order_relaxed);
    };

    if (islandJobs.size() > 1)
    {
        // INTERLEAVED because of the cost sort above: ParallelFor chunks
        // contiguously and runs chunk 0 on the CALLER, so descending cost +
        // contiguous chunks would hand the caller the whole expensive head (the
        // main thread would run most cloth rings while the workers split the
        // hair strands). Interleaving deals the sorted tasks round-robin, so
        // every chunk gets one ring and a share of the strands. See
        // TaskSystem::ParallelSchedule.
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(islandJobs.size()),
            _timedTask,
            TaskSystem::TaskPriority::High,
            TaskSystem::ParallelSchedule::Interleaved);
    }
    else if (!islandJobs.empty())
    {
        _timedTask(0);
    }
    {
        auto& _ap = Anim::AnimProfile::Get();
        // Per lane.
        (lane == Lane::Main ? _ap.chainParallelMs : _ap.previewChainParallelMs) = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - _chainPar0).count());
        // cpu/wall is the speedup this phase actually achieved. Without it,
        // "is the parallelism paying" is an opinion.
        (lane == Lane::Main ? _ap.chainCpuMs : _ap.previewChainCpuMs) = static_cast<float>(
            _chainCpuNs.load(std::memory_order_relaxed) / 1e6);
        if (lane == Lane::Main)
        {
            for (int t = 0; t < 3; ++t) _ap.chainTierCpuMs[t] = static_cast<float>(_tierNs[t].load() / 1e6);
            _ap.chainShadowOnlyCpuMs = static_cast<float>(_shadowOnlyNs.load() / 1e6);
            _ap.chainPrepCpuMs       = 0.f;   // this path does not time its preparation
        }
    }

    // The held jobs of this (small-population) path, serially.
    for (uint32_t ji = 0; ji < static_cast<uint32_t>(jobs.size()); ++ji)
        if (jobs[ji].hold) holdEntity(ji);

    // Serial epilogue: entity-wide post-sim state the parallel jobs must not touch.
    for (auto& ij : islandJobs)
    {
        if (!ij.clamped || ij.job->data->divergenceLogged) continue;
        ij.job->data->divergenceLogged = true;
        LOG_WARNING("ChainPhysics: clamped a diverging chain (runaway hair/skirt sim — "
                    "likely a bad rest reference after delete+reload). Prevented a GPU TDR.");
    }
    for (const auto& j : jobs)
    {
        if (j.hold) continue;   // held: keeps the capsules of its last solve
        // Remember this frame's collider pose so next frame can sweep the collider
        // across substeps. A capsule add / remove makes the count differ for one
        // frame => next frame falls back to the static current pose.
        if (j.capsuleCount > 0) j.data->prevCapsules.assign(j.capsules, j.capsules + j.capsuleCount);
        else                    j.data->prevCapsules.clear();
        // FillDebugSnapshot's own / scene capsule split (nothing in the solve reads it).
        j.data->prevSceneCaps = static_cast<std::uint32_t>(j.sceneCapCount);
    }
}

// ==== Invalidate: force re-detection on the entity's next pass ====
void ChainPhysicsSystem::Invalidate(Entity e)
{
    m_entityData.erase(e);
}

// ==== World-motion inertia: WantsWorldInertia / PrepareWorldKick ====
// ChainPhysicsComponent::worldInertia* / teleport*; the model is FeltKick's.
bool ChainPhysicsSystem::WantsWorldInertia(const ChainPhysicsComponent& cfg)
{
    if (cfg.worldInertiaLocation > 0.f || cfg.worldInertiaRotation > 0.f) return true;
    for (const ChainGroupDef& g : cfg.groups)   // a group's OWN value (not ovrEnabled-gated)
        if (g.ovrWorldInertiaLocation > 0.f || g.ovrWorldInertiaRotation > 0.f)
            return true;
    return false;
}

bool ChainPhysicsSystem::PrepareWorldKick(World& world, Entity e, Entity src, EntityData& d,
                                          const ChainPhysicsComponent& cfg, float dt, Lane lane,
                                          WorldKick& out)
{
    // SERIAL (RunPass phase 1): reads the World, writes this entity's record.
    const int ln = static_cast<int>(lane);
    const auto* gt = world.GetComponent<GlobalTransform>(src);
    XMVECTOR sv, q, t;
    if (!gt || !(dt > 0.f) || !std::isfinite(dt) ||
        !XMMatrixDecompose(&sv, &q, &t, XMLoadFloat4x4(&gt->matrix)))
    {
        d.wmPrevValid = d.wmDeltaValid = false;   // no world pose: nothing to difference
        return false;
    }
    q = XMQuaternionNormalize(q);
    XMFLOAT3 s3;
    XMStoreFloat3(&s3, sv);
    const float scale = (std::fabs(s3.x) + std::fabs(s3.y) + std::fabs(s3.z)) / 3.f;
    XMFLOAT4 q4; XMFLOAT3 t3;
    XMStoreFloat4(&q4, q);
    XMStoreFloat3(&t3, t);
    if (!(scale > 1e-6f) || !std::isfinite(scale) || !std::isfinite(q4.x + q4.y + q4.z + q4.w) ||
        !std::isfinite(t3.x + t3.y + t3.z))
    {
        d.wmPrevValid = d.wmDeltaValid = false;
        return false;
    }

    // Contiguous = recorded by this lane's IMMEDIATELY preceding pass. A gap
    // (anim-culled, a lane switch, the first pass after InitEntity - its
    // EntityData is new) re-records instead: motion the solver did not watch
    // is not turned into a kick. A paused / holding preview frame or a paused
    // editor runs no pass, so it is no gap.
    const bool contiguous = d.wmPrevValid && d.wmLane == static_cast<std::uint8_t>(ln) &&
                            d.wmPass + 1 == m_lanePasses[ln];
    d.wmLane = static_cast<std::uint8_t>(ln);
    d.wmPass = m_lanePasses[ln];
    auto record = [&](FXMVECTOR rq, FXMVECTOR rt, float rs)
    {
        XMStoreFloat4(&d.wmPrevRot, rq);
        XMStoreFloat3(&d.wmPrevPos, rt);
        d.wmPrevScale = rs;
        d.wmPrevValid = true;
    };
    if (!contiguous)
    {
        record(q, t, scale);
        d.wmDeltaValid = false;
        d.wmSamePasses = m_passRemaining > 1u ? m_passRemaining - 1u : 0u;
        return false;
    }
    if (d.wmSamePasses > 0u)
    {
        // An earlier pass of THIS frame recorded the frame's world sample
        // outright: this one sees the same sample - nothing to measure (the
        // record is re-stamped above, so the next frame is still contiguous).
        --d.wmSamePasses;
        return false;
    }

    // The frame's remaining entity motion, prev -> now, against the TELEPORT
    // thresholds - per solver step: an n-step preview frame divides it by the
    // passes left (the main lane has one).
    const XMVECTOR qPrev = XMLoadFloat4(&d.wmPrevRot);
    const XMVECTOR tPrev = XMLoadFloat3(&d.wmPrevPos);
    const float    steps = static_cast<float>(std::max<std::uint32_t>(1u, m_passRemaining));
    const float    dist  = XMVectorGetX(XMVector3Length(XMVectorSubtract(t, tPrev)));
    const float    cosH  = std::min(1.f, std::fabs(XMVectorGetX(XMQuaternionDot(qPrev, q))));
    const float    angDeg = 2.f * std::acos(cosH) * 57.29577951f;
    const bool teleport = (cfg.teleportDistance > 0.f && dist / steps > cfg.teleportDistance) ||
                          (cfg.teleportAngle > 0.f && angDeg / steps > cfg.teleportAngle);
    if (teleport)
    {
        // KawaiiPhysics TeleportDistance / RotationThreshold: the step's world
        // motion is IGNORED, the chains ride it rigidly with no whip. The
        // record jumps to the new pose; the frame velocity from before the
        // jump (wmDelta*) is KEPT, so an entity that keeps moving at the same
        // speed afterwards gets no stop / start kick either.
        record(q, t, scale);
        d.wmSamePasses = static_cast<std::uint32_t>(steps) - 1u;   // the rest of this frame
        ++m_worldTeleportsTotal;
        if (lane == Lane::Preview) ++m_laneStats.worldTeleports;
        if (m_worldTeleportLogs < 64)
        {
            ++m_worldTeleportLogs;
            LOG_INFO("[CHAINPHYS] world teleport: entity=%u lane=%s dist=%.4f angleDeg=%.2f "
                     "steps=%u thresholdDist=%.4f thresholdDeg=%.2f n=%llu - the step's "
                     "world motion is ignored (no whip)",
                     static_cast<unsigned>(e), lane == Lane::Preview ? "preview" : "main",
                     dist, angDeg, static_cast<unsigned>(steps), cfg.teleportDistance,
                     cfg.teleportAngle, static_cast<unsigned long long>(m_worldTeleportsTotal));
        }
        return false;
    }

    // This pass's entity pose: all of the way on a one-pass frame, 1/steps of
    // the remaining way on a preview multi-step frame.
    XMVECTOR qPass = q, tPass = t;
    float    sPass = scale;
    if (steps > 1.f)
    {
        const float f = 1.f / steps;
        if (!XMVector4Equal(qPrev, q))
            qPass = XMQuaternionNormalize(XMQuaternionSlerp(qPrev, q, f));
        tPass = XMVectorLerp(tPrev, t, f);
        sPass = d.wmPrevScale + (scale - d.wmPrevScale) * f;
    }
    // D = W_prev * inverse(W_pass), rigid part: p' = p R + T for a WORLD-fixed
    // point, R = R_prev R_pass^T, T = (t_prev - t_pass) R_pass^T / scale
    // (skeleton units). A scale CHANGE is not motion (the panel's Scale control
    // resets the chains anyway); it only sets the unit of T.
    // An unchanged orientation is an EXACT identity, which keeps FeltKick on
    // its exact translation-only path. DirectXMath's q * q^-1 happens to come
    // out exact as well, but that is its arithmetic, not a contract; this
    // compare makes it one.
    const XMVECTOR qD = XMVector4Equal(qPrev, qPass)
        ? XMQuaternionIdentity()
        : XMQuaternionNormalize(XMQuaternionMultiply(qPrev, XMQuaternionInverse(qPass)));
    const XMVECTOR tD = XMVectorScale(XMVector3InverseRotate(XMVectorSubtract(tPrev, tPass), qPass),
                                      1.f / sPass);
    const bool kick = d.wmDeltaValid;   // the first delta has no velocity to difference against
    if (kick)
    {
        XMStoreFloat4(&out.rot, qD);
        XMStoreFloat3(&out.pos, tD);
        out.dt      = dt;
        out.prevRot = d.wmDeltaRot;
        out.prevPos = d.wmDeltaPos;
        out.prevDt  = d.wmDeltaDt;
        ++m_worldKicksTotal;
        if (lane == Lane::Preview)
        {
            // Diagnostics at the entity origin, in WORLD units: speed, turn
            // rate, and the frame acceleration the kick conserves against.
            const float speed = XMVectorGetX(XMVector3Length(tD)) * sPass / dt;
            const float turn  = 2.f * std::acos(std::min(1.f, std::fabs(XMVectorGetW(qD)))) *
                                57.29577951f / dt;
            const XMVECTOR vNow  = XMVectorScale(tD, -1.f / dt);
            const XMVECTOR vPrev = XMVector3Rotate(
                XMVectorScale(XMLoadFloat3(&d.wmDeltaPos), -1.f / d.wmDeltaDt), qD);
            const float accel = XMVectorGetX(XMVector3Length(XMVectorSubtract(vNow, vPrev))) *
                                sPass / dt;
            ++m_laneStats.worldKicks;
            m_laneStats.worldMaxSpeed   = std::max(m_laneStats.worldMaxSpeed, speed);
            m_laneStats.worldMaxTurnDeg = std::max(m_laneStats.worldMaxTurnDeg, turn);
            m_laneStats.worldMaxAccel   = std::max(m_laneStats.worldMaxAccel, accel);
        }
    }
    record(qPass, tPass, sPass);
    XMStoreFloat4(&d.wmDeltaRot, qD);
    XMStoreFloat3(&d.wmDeltaPos, tD);
    d.wmDeltaDt    = dt;
    d.wmDeltaValid = true;
    return kick;
}

// The structural / live split, derived from what InitEntity READS. Its only
// config input is cfg.groups (topology: count, rootBone, type, enabled,
// excludeBones / excludeCount / excludeSubtree, ringGroup / ringIndex,
// pairGroupId; g.name is used for log lines only), and it bakes
// Constraint::restLength / Strand::maxReachSq / SpringBone::restLocalPos
// from the LocalPose ApplyRestPose has just stamped, so the authored rest
// pose (list, per-entry name / rot / weight / enabled, the master switch and
// weight) is structural too. Every other authored field is read per frame:
// the group stiffness tables are rebuilt every RunPass, Particle radius /
// friction are re-baked in Simulate, iterations / substeps / collide /
// capsules are resolved per job. Pure and static: no solver state is read
// or written.
bool ChainPhysicsSystem::NeedsReinit(const ChainPhysicsComponent& before,
                                     const ChainPhysicsComponent& after)
{
    if (before.groups.size() != after.groups.size()) return true;
    auto sameStr = [](const char* a, const char* b, std::size_t n)
    { return std::strncmp(a, b, n) == 0; };
    for (std::size_t i = 0; i < before.groups.size(); ++i)
    {
        const ChainGroupDef& a = before.groups[i];
        const ChainGroupDef& b = after.groups[i];
        if (!sameStr(a.rootBone, b.rootBone, sizeof(a.rootBone)) || a.type != b.type ||
            a.enabled != b.enabled || a.excludeCount != b.excludeCount ||
            a.excludeSubtree != b.excludeSubtree || a.ringGroup != b.ringGroup ||
            a.ringIndex != b.ringIndex || a.pairGroupId != b.pairGroupId)
            return true;
        const int n = std::min(a.excludeCount, ChainGroupDef::MAX_EXCLUDE);
        for (int j = 0; j < n; ++j)
            if (!sameStr(a.excludeBones[j], b.excludeBones[j], sizeof(a.excludeBones[j])))
                return true;
    }
    if (before.restPoseEnabled != after.restPoseEnabled ||
        before.restPoseWeight != after.restPoseWeight ||
        before.restPose.size() != after.restPose.size())
        return true;
    for (std::size_t i = 0; i < before.restPose.size(); ++i)
    {
        const BoneRestPose& a = before.restPose[i];
        const BoneRestPose& b = after.restPose[i];
        if (!sameStr(a.boneName, b.boneName, sizeof(a.boneName)) || a.rot.x != b.rot.x ||
            a.rot.y != b.rot.y || a.rot.z != b.rot.z || a.rot.w != b.rot.w ||
            a.weight != b.weight || a.enabled != b.enabled)
            return true;
    }
    return false;
}

// ==== Preview lane: public surface ====
void ChainPhysicsSystem::SetPreviewLane(const std::unordered_set<Entity>* ents, float dt,
                                        std::uint32_t steps)
{
    // A set arriving after none starts a new preview SESSION: the sway phase
    // restarts at 0 and the lane counters describe this session only (the
    // panel's closing "[CHAINPHYS] preview lane:" line is read by packs).
    if (ents && !m_previewSet)
    {
        m_previewTime = 0.f;
        m_laneStats   = PreviewLaneStats{};
        m_laneSolvedSerial[0] = m_laneSolvedSerial[1] = ~0ull;
    }
    m_previewSet = ents;
    m_previewDt  = ents ? dt : 0.f;
    // Bounded here, not trusted: a runaway caller must not turn one editor
    // frame into an unbounded serial solve.
    m_previewSteps = ents ? std::min(steps, kMaxPreviewStepsPerFrame) : 1u;
    // A lane handed back drops its preview scene, solo entries and A/B pose feeds.
    if (!ents)
    {
        m_previewGravityActive = false;
        m_previewSceneCapCount = 0;
        m_soloGroups.clear();
        m_poseFeeds.clear();
    }
}

void ChainPhysicsSystem::SetPreviewSceneCapsules(const XMFLOAT3* a, const XMFLOAT3* b,
                                                 const float* radius, std::uint32_t n)
{
    // Copied (the caller's arrays need not outlive the call) and bounded
    // here, never trusted. Serial callers only (the panel's window).
    m_previewSceneCapCount = (a && b && radius)
                           ? std::min(n, kMaxPreviewSceneCapsules) : 0u;
    for (std::uint32_t i = 0; i < m_previewSceneCapCount; ++i)
        m_previewSceneCaps[i] = WorldCapsule{ a[i], b[i], std::max(radius[i], 0.f) };
}

void ChainPhysicsSystem::ResetPreviewEntity(Entity e)
{
    // Invalidate drops the topology/Verlet state; the LocalPose scratch goes
    // too, because WriteBonePoses left SIMULATED rotations in it and a no-clip
    // skeleton is only ever seeded once - InitEntity would otherwise bake the
    // new rest lengths from the old swing. The Edit-path record describes
    // rotations in that same dropped cache (ClearAll's reasoning, per entity).
    Invalidate(e);
    m_editRestPoseApplied.erase(e);
    m_previewRecordSeen.erase(e);
    m_animSys.ClearLocalPoseCache(e);
    // A reset is a discontinuity (a scrub, a clip change): the next frame's
    // passes must not interpolate from the pose before it.
    DropPreviewPrevPose(e);
}

// ==== Preview lane: HOLD and multi-step frames ====
void ChainPhysicsSystem::HoldPass(World& world, const std::unordered_set<Entity>& set)
{
    ZoneScopedN("ChainPhysics.PreviewHold");
    std::uint32_t holdEntities = 0, heldBones = 0, diffBones = 0;
    auto* pPhys = world.GetPool<ChainPhysicsComponent>();
    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t physN = pPhys ? pPhys->Data().size() : 0;
    const auto&  physEnts = EntitiesOf(pPhys);
    for (size_t pi = 0; pi < physN; ++pi)
    {
        const Entity e = physEnts[pi];
        if (!set.count(e)) continue;
        // RunPass's phase-1 guards, in RunPass's order.
        auto* phys = &pPhys->Data()[pi];
        if (!phys->enabled) continue;
        // An A/B shadow holds on its source's skeleton, in its own fed
        // buffer (RunPass's rule).
        const Entity src = PreviewPoseSourceOf(e);
        auto* skelComp = pSkel ? pSkel->Get(src) : nullptr;
        if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle) continue;
        AnimationSystem::LocalPose* poses = PreviewPoses(e);
        if (!poses) continue;
        const SkeletonAsset& skel = m_skeletons.Get(skelComp->assetIndex);

        // Authored rest pose FIRST, as every RunPass does (serial: it
        // writes the component's runtime cache fields). AnimationSystem
        // re-sampled the clip this frame, so it has to be re-stamped.
        ChainPhysicsSystem::ApplyRestPose(*phys, skel, skelComp->assetIndex, poses);

        // No simulation state (never stepped, or just reset): what RunPass
        // does before its InitEntity, the authored rest pose and nothing else.
        auto it = m_entityData.find(e);
        if (it == m_entityData.end() || !it->second.initialized) continue;
        EntityData& data = it->second;
        if (data.strands.empty() && data.springBones.empty()) continue;

        // RunPass's no-clip resetRot over the SAME write range, skipping the
        // authored bones as RunPass does: a source-less skeleton is seeded
        // once, so poses[] still holds last frame's OUTPUT for these bones
        // and composing onto it would drift (an A/B shadow takes its
        // source's verdict).
        if (!m_animSys.HasLivePoseSource(world, src))
        {
            auto resetRot = [&](uint32_t bn)
            {
                if (bn >= skel.boneCount) return;
                if (ChainPhysicsSystem::IsRestPosedBone(*phys, bn)) return;
                XMVECTOR sc, rq, tr;
                if (XMMatrixDecompose(&sc, &rq, &tr, XMLoadFloat4x4(&skel.restPoseLocal[bn])))
                    XMStoreFloat4(&poses[bn].rot, XMQuaternionNormalize(rq));
            };
            for (const auto& strand : data.strands)
            {
                const uint32_t iBegin = strand.rootSolo ? 0u : 1u;
                for (uint32_t i = iBegin; i + 1 < strand.particleCount; ++i)
                    resetRot(data.particles[strand.particleOffset + i].boneIndex);
            }
            for (const auto& sb : data.springBones)
            {
                if (sb.isVirtual || sb.boneIndex >= skel.boneCount) continue;
                if (skel.parentIndex[sb.boneIndex] < 0) continue;
                resetRot(sb.boneIndex);
            }
        }

        // The raw (unsimulated) rotation of every bone the solver writes (the
        // pose a step-less frame would otherwise show), for holdDiffBones.
        // WriteBonePoses' range: [iBegin, particleCount-1), owners only.
        m_holdScratch.clear();
        for (const auto& strand : data.strands)
        {
            const uint32_t iBegin = strand.rootSolo ? 0u : 1u;
            for (uint32_t i = iBegin; i + 1 < strand.particleCount; ++i)
            {
                const Particle& p = data.particles[strand.particleOffset + i];
                if (!p.writesBone || p.boneIndex >= skel.boneCount) continue;
                m_holdScratch.emplace_back(p.boneIndex, poses[p.boneIndex].rot);
            }
        }
        for (const auto& sb : data.springBones)
        {
            if (sb.isVirtual || sb.boneIndex >= skel.boneCount) continue;
            if (skel.parentIndex[sb.boneIndex] < 0) continue;
            m_holdScratch.emplace_back(sb.boneIndex, poses[sb.boneIndex].rot);
        }

        // Re-write from the particles / springs AS THEY ARE, island by island in
        // Simulate's order (strands, then the springs island). Islands are
        // FK-disjoint (BuildIslands), so the serial order reproduces the
        // parallel pass exactly.
        for (const Island& isl : data.islands)
        {
            if (isl.strandEnd > isl.strandBegin)
                WriteBonePoses(data, isl, poses, skel);
            if (isl.springs && !data.springBones.empty())
                WriteSpringBonePoses(data, poses, skel);
        }

        ++holdEntities;
        heldBones += static_cast<std::uint32_t>(m_holdScratch.size());
        for (const auto& [bone, before] : m_holdScratch)
        {
            const XMFLOAT4& after = poses[bone].rot;
            const float d = before.x * after.x + before.y * after.y +
                            before.z * after.z + before.w * after.w;
            if (1.f - std::fabs(d) > 1e-7f) ++diffBones;
        }
    }
    m_laneStats.holdEntities  = holdEntities;
    m_laneStats.heldBones     = heldBones;
    m_laneStats.holdDiffBones = diffBones;
}

void ChainPhysicsSystem::SnapshotPreviewPoses(World& world, const std::unordered_set<Entity>& set)
{
    // Pool-driven: the preview set's chain carriers only - the entities RunPass
    // writes poses for. Every bone is copied (the whole input pose), so the
    // restore needs no knowledge of which bones a pass touched.
    m_previewPoseSnapCount = 0;
    auto* pPhys = world.GetPool<ChainPhysicsComponent>();
    auto* pSkel = world.GetPool<SkeletonComponent>();
    if (!pPhys) return;
    for (Entity e : pPhys->Entities())
    {
        if (!set.count(e)) continue;
        // An A/B shadow's input is its fed buffer, on its source's
        // skeleton (a shadow's own pose is never AnimationSystem's).
        const SkeletonComponent* sk = pSkel ? pSkel->Get(PreviewPoseSourceOf(e)) : nullptr;
        if (!sk || sk->assetIndex == kInvalidAnimHandle) continue;
        const AnimationSystem::LocalPose* poses = PreviewPosesConst(e);
        if (!poses) continue;
        const uint32_t bones = m_skeletons.Get(sk->assetIndex).boneCount;
        if (m_previewPoseSnapCount == m_previewPoseSnap.size())
            m_previewPoseSnap.emplace_back();
        PreviewPoseSnap& s = m_previewPoseSnap[m_previewPoseSnapCount++];
        s.e = e;
        s.serial = m_updateSerial;
        s.pose.assign(poses, poses + bones);
    }
}

void ChainPhysicsSystem::RestorePreviewPoses()
{
    for (std::size_t i = 0; i < m_previewPoseSnapCount; ++i)
    {
        const PreviewPoseSnap& s = m_previewPoseSnap[i];
        // Re-fetched, never cached across the pass: the pointer is the
        // AnimationSystem's to move (a shadow's is its feed buffer).
        if (AnimationSystem::LocalPose* poses = PreviewPoses(s.e))
            std::copy(s.pose.begin(), s.pose.end(), poses);
    }
}

std::uint32_t ChainPhysicsSystem::WriteInterpolatedPreviewPoses(float frac)
{
    std::uint32_t interpolated = 0;
    for (std::size_t i = 0; i < m_previewPoseSnapCount; ++i)
    {
        const PreviewPoseSnap& cur = m_previewPoseSnap[i];
        AnimationSystem::LocalPose* poses = PreviewPoses(cur.e);   // a shadow's: its feed buffer
        if (!poses) continue;
        // The origin must be THIS entity's input from the IMMEDIATELY preceding
        // Update (a stepping or holding preview frame): anything older, or a
        // pose of another length, is not the start of this frame's interval.
        const PreviewPoseSnap* prev = nullptr;
        for (std::size_t j = 0; j < m_previewPrevCount; ++j)
            if (m_previewPrevPose[j].e == cur.e) { prev = &m_previewPrevPose[j]; break; }
        if (!prev || prev->serial + 1 != m_updateSerial || prev->pose.size() != cur.pose.size())
        {
            std::copy(cur.pose.begin(), cur.pose.end(), poses);
            continue;
        }
        const std::size_t bones = cur.pose.size();
        for (std::size_t b = 0; b < bones; ++b)
        {
            const AnimationSystem::LocalPose& a = prev->pose[b];
            const AnimationSystem::LocalPose& c = cur.pose[b];
            // A bone that did not move over the frame is copied, not blended:
            // a still pose stays bit-exact (and most of a skeleton is still).
            if (std::memcmp(&a, &c, sizeof(a)) == 0) { poses[b] = c; continue; }
            XMStoreFloat3(&poses[b].pos, XMVectorLerp(XMLoadFloat3(&a.pos), XMLoadFloat3(&c.pos), frac));
            XMStoreFloat3(&poses[b].scl, XMVectorLerp(XMLoadFloat3(&a.scl), XMLoadFloat3(&c.scl), frac));
            XMStoreFloat4(&poses[b].rot, XMQuaternionSlerp(XMLoadFloat4(&a.rot), XMLoadFloat4(&c.rot), frac));
        }
        ++interpolated;
    }
    return interpolated;
}

void ChainPhysicsSystem::KeepPreviewInputAsPrevious()
{
    // Swap, not copy: both vectors keep their capacity, so a steady preview
    // allocates nothing per frame.
    std::swap(m_previewPoseSnap, m_previewPrevPose);
    std::swap(m_previewPoseSnapCount, m_previewPrevCount);
    m_previewPoseSnapCount = 0;
}

void ChainPhysicsSystem::DropPreviewPrevPose(Entity e)
{
    for (std::size_t j = 0; j < m_previewPrevCount; ++j)
        if (m_previewPrevPose[j].e == e) m_previewPrevPose[j].e = NullEntity;
}

std::uint32_t ChainPhysicsSystem::CountPreviewRestPoseRecords() const
{
    std::uint32_t records = 0;
    if (m_previewSet)
        for (Entity e : *m_previewSet) records += m_editRestPoseApplied.count(e) ? 1u : 0u;
    return records;
}

void ChainPhysicsSystem::LogPreviewLaneStats(const char* when) const
{
    const std::uint32_t records = CountPreviewRestPoseRecords();
    const PreviewLaneStats& s = m_laneStats;
    LOG_INFO("[CHAINPHYS] preview lane: entities=%u substeps=%u excludedByPreviewLane=%u "
             "solvedBothLanes=%llu restPoseRecordChurn=%llu restPoseRecords=%u "
             "swayTime=%.3f dt=%.5f passes=%llu pausedFrames=%llu frames=%llu "
             "serialMs=%.3f parallelMs=%.3f cpuMs=%.3f tier=%u iterations=%u "
             "steps=%u maxStepsPerFrame=%u interpolatedPasses=%llu holdFrames=%llu "
             "holdEntities=%u heldBones=%u holdDiffBones=%u sceneCapsules=%u "
             "gravityOverride=%u worldKicks=%llu worldTeleports=%llu worldMaxSpeed=%.4f "
             "worldMaxTurnDeg=%.3f worldMaxAccel=%.3f worldSuppressed=%d when=%s",
             s.entities, s.substeps, s.excludedByPreviewLane,
             static_cast<unsigned long long>(s.solvedBothLanes),
             static_cast<unsigned long long>(s.restPoseRecordChurn), records,
             m_previewTime, m_previewDt,
             static_cast<unsigned long long>(s.passes),
             static_cast<unsigned long long>(s.pausedFrames),
             static_cast<unsigned long long>(s.frames),
             s.serialMs, s.parallelMs, s.cpuMs,
             // session max LOD tier / min iterations (0 = no pass ran)
             s.maxTier, s.minIterations == ~0u ? 0u : s.minIterations,
             // this frame's passes, the session's most in one frame, and
             // the hold's counters (the last hold's entities / bones / bones
             // whose held rotation differs from the raw clip pose)
             s.steps, s.maxStepsPerFrame,
             static_cast<unsigned long long>(s.interpolatedPasses),
             static_cast<unsigned long long>(s.holdFrames), s.holdEntities,
             s.heldBones, s.holdDiffBones, s.sceneCapsules, s.gravityOverride,
             // world-motion inertia: the session's kicks / teleports and the
             // largest entity speed / turn rate / acceleration a kick saw
             static_cast<unsigned long long>(s.worldKicks),
             static_cast<unsigned long long>(s.worldTeleports),
             s.worldMaxSpeed, s.worldMaxTurnDeg, s.worldMaxAccel,
             m_worldInertiaSuppressed ? 1 : 0,
             when ? when : "");
}

bool ChainPhysicsSystem::CopyParticleState(Entity e, std::vector<DirectX::XMFLOAT3>* positions,
                                           std::vector<DirectX::XMFLOAT3>* restPositions) const
{
    auto it = m_entityData.find(e);
    if (it == m_entityData.end() || !it->second.initialized) return false;
    const std::vector<Particle>& ps = it->second.particles;
    if (positions)
    {
        positions->resize(ps.size());
        for (size_t i = 0; i < ps.size(); ++i) (*positions)[i] = ps[i].position;
    }
    if (restPositions)
    {
        restPositions->resize(ps.size());
        for (size_t i = 0; i < ps.size(); ++i) (*restPositions)[i] = ps[i].restPosition;
    }
    return true;
}

// ==== Debug snapshot: SegmentSegmentDistance / FillDebugSnapshot ====
float ChainPhysicsSystem::SegmentSegmentDistance(const XMFLOAT3& p1, const XMFLOAT3& q1,
                                                 const XMFLOAT3& p2, const XMFLOAT3& q2)
{
    // Ericson, Real-Time Collision Detection 5.1.9 (ClosestPtSegmentSegment).
    const XMVECTOR P1 = XMLoadFloat3(&p1), Q1 = XMLoadFloat3(&q1);
    const XMVECTOR P2 = XMLoadFloat3(&p2), Q2 = XMLoadFloat3(&q2);
    const XMVECTOR d1 = XMVectorSubtract(Q1, P1);
    const XMVECTOR d2 = XMVectorSubtract(Q2, P2);
    const XMVECTOR r  = XMVectorSubtract(P1, P2);
    const float a = XMVectorGetX(XMVector3Dot(d1, d1));
    const float e = XMVectorGetX(XMVector3Dot(d2, d2));
    const float f = XMVectorGetX(XMVector3Dot(d2, r));
    constexpr float kEps = 1e-12f;
    float s = 0.f, t = 0.f;
    if (a <= kEps && e <= kEps)
        return XMVectorGetX(XMVector3Length(r));
    if (a <= kEps)
        t = std::clamp(f / e, 0.f, 1.f);
    else
    {
        const float c = XMVectorGetX(XMVector3Dot(d1, r));
        if (e <= kEps)
            s = std::clamp(-c / a, 0.f, 1.f);
        else
        {
            const float b     = XMVectorGetX(XMVector3Dot(d1, d2));
            const float denom = a * e - b * b;
            s = (denom > kEps) ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f)      { t = 0.f; s = std::clamp(-c / a, 0.f, 1.f); }
            else if (t > 1.f) { t = 1.f; s = std::clamp((b - c) / a, 0.f, 1.f); }
        }
    }
    const XMVECTOR c1 = XMVectorAdd(P1, XMVectorScale(d1, s));
    const XMVECTOR c2 = XMVectorAdd(P2, XMVectorScale(d2, t));
    return XMVectorGetX(XMVector3Length(XMVectorSubtract(c1, c2)));
}

// Read-only: a copy of what the last pass left in m_entityData, plus three
// derived read-outs (the rest-limit state per particle, the LOD-skipped
// collide groups, the capsule interpenetration count). Nothing in the solve
// reads any of it.
bool ChainPhysicsSystem::FillDebugSnapshot(Entity e, DebugSnapshot& out,
                                           const ChainPhysicsComponent* cfg) const
{
    out = DebugSnapshot{};
    auto it = m_entityData.find(e);
    if (it == m_entityData.end() || !it->second.initialized) return false;
    const EntityData& d = it->second;
    const std::size_t np = d.particles.size();

    out.particlePos.resize(np);
    out.particleRadius.resize(np);
    out.particleBone.resize(np);
    out.particleWritesBone.assign(np, ~0u);
    out.particleIsland.assign(np, 0xFF);
    out.particleGroup.assign(np, 0xFF);
    out.particlePinned.resize(np);
    out.particleLimitViolated.assign(np, 0);
    for (std::size_t i = 0; i < np; ++i)
    {
        const Particle& p = d.particles[i];
        out.particlePos[i]    = p.position;
        out.particleRadius[i] = p.radius;
        out.particleBone[i]   = p.boneIndex;
        out.particlePinned[i] = p.invMass == 0.f ? 1 : 0;
    }
    out.islands = static_cast<std::uint32_t>(d.islands.size());
    for (std::size_t k = 0; k < d.islands.size(); ++k)
    {
        const Island& isl = d.islands[k];
        for (std::uint32_t i = isl.particleBegin; i < isl.particleEnd && i < np; ++i)
            out.particleIsland[i] = static_cast<std::uint8_t>(std::min<std::size_t>(k, 0xFE));
    }

    // Strands: group, the write ownership (WriteBonePoses' range exactly:
    // [rootSolo ? 0 : 1, particleCount - 1) and Particle::writesBone), and the
    // rest-limit state (ApplyRestLimit's cone, measured on the FINAL positions
    // against this frame's animated rest - restPosition).
    out.strands = static_cast<std::uint32_t>(d.strands.size());
    std::vector<std::uint8_t> groupSeen(256, 0);
    for (const Strand& s : d.strands)
    {
        if (s.particleOffset + s.particleCount > np) continue;
        if (s.groupIdx != 0xFF) groupSeen[s.groupIdx] = 1;
        const std::uint32_t iBegin = s.rootSolo ? 0u : 1u;
        for (std::uint32_t i = 0; i < s.particleCount; ++i)
        {
            const std::uint32_t pi = s.particleOffset + i;
            out.particleGroup[pi] = s.groupIdx;
            if (i >= iBegin && i + 1 < s.particleCount && d.particles[pi].writesBone)
                out.particleWritesBone[pi] = d.particles[pi].boneIndex;
        }
        if (!cfg || s.particleCount < 2) continue;
        const ChainGroupDef* o = (s.groupIdx < cfg->groups.size() && cfg->groups[s.groupIdx].ovrEnabled)
                                     ? &cfg->groups[s.groupIdx] : nullptr;
        const bool isSkirt = (s.type == StrandType::Skirt);
        const float limDeg = o ? o->ovrLimitAngle : (isSkirt ? cfg->skirtLimitAngle : cfg->limitAngle);
        if (limDeg <= 0.f || limDeg >= 180.f) continue;
        const float tipScale = std::max(o ? o->ovrLimitTipScale
                                          : (isSkirt ? cfg->skirtLimitTipScale : cfg->limitTipScale), 0.f);
        const float chainLen = static_cast<float>(s.particleCount);
        for (std::uint32_t i = 1; i < s.particleCount; ++i)
        {
            const Particle& p   = d.particles[s.particleOffset + i];
            const Particle& par = d.particles[s.particleOffset + i - 1];
            XMVECTOR rest = XMVectorSubtract(XMLoadFloat3(&p.restPosition), XMLoadFloat3(&par.restPosition));
            XMVECTOR cur  = XMVectorSubtract(XMLoadFloat3(&p.position), XMLoadFloat3(&par.position));
            if (XMVectorGetX(XMVector3LengthSq(rest)) < 1e-12f ||
                XMVectorGetX(XMVector3LengthSq(cur)) < 1e-12f) continue;
            rest = XMVector3Normalize(rest);
            cur  = XMVector3Normalize(cur);
            const float t = static_cast<float>(i) / (chainLen - 1.f);
            float half = limDeg * (1.f + t * (tipScale - 1.f));
            half = std::min(std::max(half, 0.f), 179.f);
            const float dotv = std::clamp(XMVectorGetX(XMVector3Dot(cur, rest)), -1.f, 1.f);
            const float angDeg = std::acos(dotv) * 57.29577951f;
            std::uint8_t& flag = out.particleLimitViolated[s.particleOffset + i];
            if (angDeg > half + kLimitTouchDeg)       { flag = 2; ++out.limitBeyond; }
            else if (angDeg >= half - kLimitTouchDeg) { flag = 1; ++out.limitOnCone; }
        }
    }
    for (int g = 0; g < 256; ++g) out.groupsSimulated += groupSeen[g];
    out.springBones = static_cast<std::uint32_t>(d.springBones.size());

    // Constraints, per colour bucket (0-1 vertical, 2-3 ring, 4-5 shear).
    for (int g = 0; g < kMaxGroups; ++g)
        for (const Constraint& c : d.constraintGroups[g])
        {
            if (c.particleA >= np || c.particleB >= np) continue;
            out.links.emplace_back(c.particleA, c.particleB);
            out.linkBucket.push_back(static_cast<std::uint8_t>(g));
        }

    // LOD: tier 2 removes collision for the whole entity (RunPass's job.collide).
    out.lodTier = d.lodTier < 0 ? 0 : d.lodTier;
    if (cfg && out.lodTier >= 2)
        for (std::size_t g = 0; g < cfg->groups.size() && g < groupSeen.size(); ++g)
            if (groupSeen[g] && cfg->groups[g].enabled && cfg->groups[g].collide)
                ++out.skippedCollideGroups;

    // The last pass's colliders: the entity's own first, then the preview
    // scene's tail (prevSceneCaps). Interpenetration is a TRUE
    // capsule-capsule test, between the entity's own capsules only.
    const std::uint32_t nCap  = static_cast<std::uint32_t>(d.prevCapsules.size());
    const std::uint32_t nOwn  = nCap - std::min(nCap, d.prevSceneCaps);
    out.ownCapsules = nOwn;
    out.capsules.resize(nCap);
    for (std::uint32_t c = 0; c < nCap; ++c)
    {
        out.capsules[c].a = d.prevCapsules[c].a;
        out.capsules[c].b = d.prevCapsules[c].b;
        out.capsules[c].radius = d.prevCapsules[c].radius;
        out.capsules[c].scene = c >= nOwn ? 1 : 0;
        out.capsules[c].interpenetrating = 0;
    }
    for (std::uint32_t i = 0; i < nOwn; ++i)
        for (std::uint32_t j = i + 1; j < nOwn; ++j)
        {
            const WorldCapsule& A = d.prevCapsules[i];
            const WorldCapsule& B = d.prevCapsules[j];
            if (SegmentSegmentDistance(A.a, A.b, B.a, B.b) < A.radius + B.radius)
            {
                ++out.interpenetratingPairs;
                out.capsules[i].interpenetrating = 1;
                out.capsules[j].interpenetrating = 1;
            }
        }
    for (std::uint32_t c = 0; c < nOwn; ++c) out.interpenetratingCapsules += out.capsules[c].interpenetrating;

    // The solo (what ApplySoloFreeze froze) and how far each population
    // sits from this frame's animated rest: frozen particles must read
    // exactly 0 (the "frozen AT REST" contract), the simulated ones not.
    out.particleFrozen.assign(np, 0);
    out.soloGroup       = d.soloGroup;
    out.frozenStrands   = d.frozenStrands;
    out.frozenGroups    = d.frozenGroups;
    out.frozenParticles = d.frozenParticles;
    out.frozenSprings   = d.frozenSprings;
    for (const Strand& s : d.strands)
    {
        if (s.particleOffset + s.particleCount > np) continue;
        for (std::uint32_t i = 0; i < s.particleCount; ++i)
        {
            const std::uint32_t pi = s.particleOffset + i;
            const Particle& p = d.particles[pi];
            const float dx = p.position.x - p.restPosition.x, dy = p.position.y - p.restPosition.y,
                        dz = p.position.z - p.restPosition.z;
            const float dev = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (s.frozen)
            {
                out.particleFrozen[pi] = 1;
                out.frozenMaxDeviation = std::max(out.frozenMaxDeviation, dev);
            }
            else if (p.invMass != 0.f)
                out.activeMaxDeviation = std::max(out.activeMaxDeviation, dev);
        }
    }
    out.pendingImpulses = static_cast<std::uint32_t>(d.impulses.size());
    return true;
}

// ==== Preview lane: per-group solo, poke, the A/B pose feed ====
// Every entry point is SERIAL (the panel's ProcessPendingActions window);
// everything they store is read by Lane::Preview passes only.
void ChainPhysicsSystem::SetSoloGroup(Entity e, int groupIdx)
{
    const int g = groupIdx < 0 ? -1 : groupIdx;
    const auto it = m_soloGroups.find(e);
    const int was = it == m_soloGroups.end() ? -1 : it->second;
    if (g < 0) m_soloGroups.erase(e);
    else       m_soloGroups[e] = g;
    // InitEntity applies the freeze (ApplySoloFreeze), so a change
    // re-initialises the entity through the single-entity reset (Invalidate +
    // the LocalPose scratch: InitEntity must bake from a clean pose, not from
    // the last swing a no-clip rig holds).
    if (was != g) ResetPreviewEntity(e);
}

int ChainPhysicsSystem::GetSoloGroup(Entity e) const
{
    const auto it = m_soloGroups.find(e);
    return it == m_soloGroups.end() ? -1 : it->second;
}

void ChainPhysicsSystem::ApplySoloFreeze(Entity e, EntityData& data)
{
    data.soloGroup = -1;
    data.frozenStrands = data.frozenGroups = data.frozenParticles = data.frozenSprings = 0;
    const auto it = m_soloGroups.find(e);
    if (it == m_soloGroups.end() || it->second < 0) return;
    const int solo = it->second;
    data.soloGroup = solo;
    bool groupFrozen[256] = {};
    // FROZEN AT REST: every particle of every other group's strands becomes
    // kinematic (invMass 0: the integration, the PBD and the contact passes
    // leave pinned particles alone; Simulate moves them with the animated pose
    // and snaps them onto it before the pose write). A strand with no group
    // (0xFF, the keyword path) is not the solo group either, so it freezes too.
    for (Strand& s : data.strands)
    {
        if (static_cast<int>(s.groupIdx) == solo) continue;
        s.frozen = true;
        ++data.frozenStrands;
        groupFrozen[s.groupIdx] = true;
        for (uint32_t i = 0; i < s.particleCount && s.particleOffset + i < data.particles.size(); ++i)
        {
            data.particles[s.particleOffset + i].invMass = 0.f;
            ++data.frozenParticles;
        }
    }
    for (SpringBone& sb : data.springBones)
    {
        if (static_cast<int>(sb.groupIdx) == solo) continue;
        sb.frozen = true;
        ++data.frozenSprings;
        groupFrozen[sb.groupIdx] = true;
    }
    for (int g = 0; g < 255; ++g) data.frozenGroups += groupFrozen[g] ? 1u : 0u;   // 0xFF = no group
}

bool ChainPhysicsSystem::ApplyExternalImpulse(Entity e, std::uint32_t particleIdx,
                                              const XMFLOAT3& impulse, float blendSeconds)
{
    // PREVIEW LANE ONLY: the queue is consumed by Lane::Preview passes alone.
    if (!m_previewSet || !m_previewSet->count(e)) return false;
    const auto it = m_entityData.find(e);
    if (it == m_entityData.end() || !it->second.initialized) return false;
    EntityData& d = it->second;
    if (particleIdx >= d.particles.size() || d.particles[particleIdx].invMass == 0.f) return false;
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z) ||
        !std::isfinite(blendSeconds))
        return false;
    // The TTL: the impulse is spread over `blend` seconds of SIMULATED time and
    // is gone after it (PrepareExternalImpulses). Clamped so no caller can
    // queue a near-infinite spike or a near-permanent force.
    const float blend = std::clamp(blendSeconds, kImpulseMinBlend, kImpulseMaxTtl);
    if (d.impulses.size() >= kMaxPendingImpulses) d.impulses.erase(d.impulses.begin());
    EntityData::PendingImpulse im;
    im.particle  = particleIdx;
    im.accel     = { impulse.x / blend, impulse.y / blend, impulse.z / blend };
    im.remaining = blend;
    im.born      = m_updateSerial;
    d.impulses.push_back(im);
    return true;
}

std::uint32_t ChainPhysicsSystem::GetPendingImpulseCount(Entity e) const
{
    const auto it = m_entityData.find(e);
    return it == m_entityData.end() ? 0u : static_cast<std::uint32_t>(it->second.impulses.size());
}

bool ChainPhysicsSystem::PrepareExternalImpulses(EntityData& data, float dt)
{
    if (data.impulses.empty()) return false;
    // The time THIS pass integrates (Simulate clamps its dt the same way): the
    // acceleration is constant across the pass's substeps, so the velocity it
    // adds is accel * cdt * (share / cdt) = accel * share, and the shares of an
    // impulse sum to its blend time - i.e. to impulse in total.
    const float cdt = std::min(dt, 1.f / 30.f);
    if (!(cdt > 0.f)) return false;
#if WITH_HARNESS
    // -chainpreview_poke_ttl_off: the REVERSE ARM of chain_preview_poke - an
    // impulse that never expires (a permanent force). Harness-only.
    static const bool s_ttlOff = Sys::CommandLine::Get().Has("-chainpreview_poke_ttl_off");
#else
    constexpr bool s_ttlOff = false;
#endif
    data.extAccel.assign(data.particles.size(), XMFLOAT3{ 0.f, 0.f, 0.f });
    bool any = false;
    for (std::size_t k = 0; k < data.impulses.size();)
    {
        EntityData::PendingImpulse& im = data.impulses[k];
        // Unconsumed for too long (the lane held / paused the whole time), or
        // a particle index a re-init moved away from: dropped, never applied late.
        const bool stale = !s_ttlOff && m_updateSerial > im.born + kImpulseMaxAgeUpdates;
        if (stale || im.particle >= data.particles.size())
        {
            data.impulses.erase(data.impulses.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        const float share = std::min(im.remaining, cdt);
        const float f = share / cdt;
        XMFLOAT3& a = data.extAccel[im.particle];
        a.x += im.accel.x * f;
        a.y += im.accel.y * f;
        a.z += im.accel.z * f;
        any = true;
        if (!s_ttlOff) im.remaining -= share;
        if (im.remaining <= 1e-6f)
            data.impulses.erase(data.impulses.begin() + static_cast<std::ptrdiff_t>(k));
        else
            ++k;
    }
    return any;
}

void ChainPhysicsSystem::SetPreviewPoseFeed(Entity shadow, Entity source)
{
    if (shadow == NullEntity) return;
    for (std::size_t i = 0; i < m_poseFeeds.size(); ++i)
        if (m_poseFeeds[i].shadow == shadow)
        {
            if (source == NullEntity || source == shadow)
                m_poseFeeds.erase(m_poseFeeds.begin() + static_cast<std::ptrdiff_t>(i));
            else
                m_poseFeeds[i].source = source;
            return;
        }
    if (source == NullEntity || source == shadow) return;
    PoseFeed f;
    f.shadow = shadow;
    f.source = source;
    m_poseFeeds.push_back(std::move(f));
}

ChainPhysicsSystem::PoseFeed* ChainPhysicsSystem::FindPoseFeed(Entity shadow)
{
    for (PoseFeed& f : m_poseFeeds)
        if (f.shadow == shadow) return &f;
    return nullptr;
}

const ChainPhysicsSystem::PoseFeed* ChainPhysicsSystem::FindPoseFeed(Entity shadow) const
{
    for (const PoseFeed& f : m_poseFeeds)
        if (f.shadow == shadow) return &f;
    return nullptr;
}

Entity ChainPhysicsSystem::PreviewPoseSourceOf(Entity e) const
{
    if (m_poseFeeds.empty()) return e;
    const PoseFeed* f = FindPoseFeed(e);
    return f ? f->source : e;
}

AnimationSystem::LocalPose* ChainPhysicsSystem::PreviewPoses(Entity e)
{
    if (!m_poseFeeds.empty())
        if (PoseFeed* f = FindPoseFeed(e))
            return f->pose.empty() ? nullptr : f->pose.data();
    return m_animSys.GetMutableLocalPose(e);
}

const AnimationSystem::LocalPose* ChainPhysicsSystem::PreviewPosesConst(Entity e) const
{
    if (!m_poseFeeds.empty())
        if (const PoseFeed* f = FindPoseFeed(e))
            return f->pose.empty() ? nullptr : f->pose.data();
    return m_animSys.GetLocalPose(e);
}

void ChainPhysicsSystem::FeedPreviewPoses(World& world)
{
    // The source's pose NOW is AnimationSystem's output for this frame (the
    // clip sampled / the rest seeded) - the preview passes below have not run,
    // and the main lane never touches a preview entity. A source with no pose
    // (culled, just reset) empties the buffer: the shadow sits the frame out,
    // exactly as its source's own pass would.
    auto* pSkel = world.GetPool<SkeletonComponent>();
    for (PoseFeed& f : m_poseFeeds)
    {
        const SkeletonComponent* sk = pSkel ? pSkel->Get(f.source) : nullptr;
        const AnimationSystem::LocalPose* src = m_animSys.GetLocalPose(f.source);
        if (!sk || sk->assetIndex == kInvalidAnimHandle || !src) { f.pose.clear(); continue; }
        const uint32_t bones = m_skeletons.Get(sk->assetIndex).boneCount;
        f.pose.assign(src, src + bones);
    }
}

void ChainPhysicsSystem::DropPreviewM7(Entity e)
{
    m_soloGroups.erase(e);
    for (std::size_t i = 0; i < m_poseFeeds.size();)
    {
        if (m_poseFeeds[i].shadow == e || m_poseFeeds[i].source == e)
            m_poseFeeds.erase(m_poseFeeds.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

// ==== GetBoneWorldMatrices: editor overlay query (read-only, per-frame cache) ====
// Returns the bone world matrices RunPass rebuilds every pass for an
// initialized entity, or nullptr while no simulation data exists (e.g. right
// after Invalidate, before the next pass rebuilds the cache). Bone b's
// position is (m._41, m._42, m._43) of matrices[b].
const std::vector<DirectX::XMFLOAT4X4>*
ChainPhysicsSystem::GetBoneWorldMatrices(Entity e) const
{
    auto it = m_entityData.find(e);
    if (it == m_entityData.end()) return nullptr;
    const EntityData& d = it->second;
    if (!d.initialized || d.boneWorldMatCache.empty()) return nullptr;
    return &d.boneWorldMatCache;
}

// ==== BuildGroupsFromNames: the editor's "Auto-detect from bone names" ====
// Runs the keyword classification (ClassifyBone / ParseSkirtID) once and
// fills cfg.groups with explicit, editable ChainGroupDef entries that
// reproduce the keyword path's result: one Chain group per hair root, one
// Cloth group per skirt root (ring ids copied from ParseSkirtID, so the ring
// topology and order are kept), one Spring group per jiggle bone (chest bones
// sharing a parent share a pairGroupId, so they re-pair under a virtual
// root). The keyword logic thus lives in exactly one place.
void ChainPhysicsSystem::BuildGroupsFromNames(const SkeletonAsset& skel,
                                              ChainPhysicsComponent& cfg)
{
    cfg.groups.clear();

    std::vector<int> boneClass(skel.boneCount, 0);
    for (uint32_t b = 0; b < skel.RealBoneCount(); ++b)   // a virtual bone is never a chain
        boneClass[b] = ClassifyBone(skel.boneNames[b]);

    auto isPhys = [&](int32_t b) {
        return b >= 0 && static_cast<uint32_t>(b) < skel.boneCount && boneClass[b] != 0;
    };
    auto addGroup = [&](uint32_t root, ChainGroupType type,
                        int ringG, int ringI, int pair)
    {
        if (cfg.groups.size() >= static_cast<size_t>(ChainPhysicsComponent::MAX_GROUPS)) return;
        ChainGroupDef g{};
        snprintf(g.name,     sizeof(g.name),     "%s", skel.boneNames[root]);
        snprintf(g.rootBone, sizeof(g.rootBone), "%s", skel.boneNames[root]);
        g.type        = type;
        g.ringGroup   = ringG;
        g.ringIndex   = ringI;
        g.pairGroupId = pair;
        cfg.groups.push_back(g);
    };

    // Hair + skirt: chain roots = phys bone whose parent is NOT a phys bone.
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        if (boneClass[b] == 1 && !isPhys(skel.parentIndex[b]))
            addGroup(b, ChainGroupType::Chain, 0, -1, -1);
        else if (boneClass[b] == 2 && !isPhys(skel.parentIndex[b]))
        {
            SkirtID sid = ParseSkirtID(skel.boneNames[b]);
            addGroup(b, ChainGroupType::Cloth, sid.group, sid.chain, -1);
        }
    }

    // Spring: one group per class-3 bone; chest bones sharing a parent share a
    // pairGroupId (encoded as the parent index) to recreate the virtual root.
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        if (boneClass[b] != 3) continue;
        std::string nm(skel.boneNames[b]);
        const bool isChest = nm.find("Chest") != std::string::npos ||
                             nm.find("chest") != std::string::npos;
        const int pair = (isChest && skel.parentIndex[b] >= 0)
                       ? skel.parentIndex[b] : -1;
        addGroup(b, ChainGroupType::Spring, 0, -1, pair);
    }

    if (cfg.groups.size() >= static_cast<size_t>(ChainPhysicsComponent::MAX_GROUPS))
        LOG_WARNING("ChainPhysics: BuildGroupsFromNames hit MAX_GROUPS (%d) — some chains "
                    "were not captured; consolidate skirt panels under one root group + excludes.",
                    ChainPhysicsComponent::MAX_GROUPS);
}
