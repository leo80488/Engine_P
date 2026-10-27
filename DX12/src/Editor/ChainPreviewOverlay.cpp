// ChainPreviewOverlay.cpp - see the header for the contract (two halves, the
// projection convention, the depth-fade stand-in for occlusion).
//
// The primitive list is the main viewport overlay's
// (EditorLayer::EmitChainPhysicsOverlay: root / chain spheres, parent links,
// excluded crosses, authored-rest markers, the focused group's limit cones)
// plus the chain solver's capsule colliders, re-expressed as screen-space ImGui
// primitives. The bone classification mirrors InitEntity's (root + subtree -
// excludes, excludeSubtree), exactly as the main overlay does.
//
// Editor-only TU (EditorCore).

#include "Editor/ChainPreviewOverlay.h"
#include "Editor/DrawListRaster.h"          // the shot's software raster

#include "ECS/AnimationComponents.h"      // SkeletonComponent / SkeletonRef
#include "ECS/HierarchyComponents.h"
#include "Physics/ChainPhysicsSystem.h"   // ChainPhysicsComponent / CapsuleColliderComponent
#include "Renderer/Renderer.h"            // GetSkeletonRegistry
#include "Resource/SkeletonAsset.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace Editor
{
namespace ChainOverlay
{
using namespace DirectX;

const char* BitName(std::uint32_t i)
{
    static const char* kNames[kBitCount] = {
        "roots", "chain", "links", "excludes", "restPose", "cone", "capsules",
        // the solver's state
        "particles", "constraints", "islands", "limits", "ownership", "lodSkip", "interpen",
        // the A/B ghost
        "ghost" };
    return i < kBitCount ? kNames[i] : "?";
}

namespace
{
    XMFLOAT3 BonePos(const std::vector<XMFLOAT4X4>& b2m, std::uint32_t b, FXMMATRIX ent)
    {
        XMFLOAT3 o;
        XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorZero(),
                                                  XMMatrixMultiply(XMLoadFloat4x4(&b2m[b]), ent)));
        return o;
    }

    std::uint32_t ResolveBone(const SkeletonAsset& skel, const char* nm)
    {
        if (!nm || !nm[0]) return ~0u;
        for (std::uint32_t b = 0; b < skel.boneCount; ++b)
            if (std::strcmp(skel.boneNames[b], nm) == 0) return b;
        return ~0u;
    }

    float EntityScale(FXMMATRIX m)
    {
        const float sx = XMVectorGetX(XMVector3Length(m.r[0]));
        const float sy = XMVectorGetX(XMVector3Length(m.r[1]));
        const float sz = XMVectorGetX(XMVector3Length(m.r[2]));
        const float s  = std::max(sx, std::max(sy, sz));
        return (s > 1e-6f && std::isfinite(s)) ? s : 1.f;
    }
}

// ==== Game half: the snapshot ====
bool SkeletonToWorld(Renderer& renderer, World& world, Entity chain,
                     const BoneFn& bones, XMFLOAT4X4& out)
{
    if (chain == NullEntity || !world.IsAlive(chain) || !bones) return false;
    std::vector<XMFLOAT4X4> b2m;
    XMMATRIX ent = XMMatrixIdentity();
    if (!bones(renderer, world, chain, b2m, ent)) return false;
    XMStoreFloat4x4(&out, ent);
    return true;
}

bool BuildSnapshot(Renderer& renderer, World& world, Entity chain,
                   const ChainPhysicsComponent& cfg,
                   const std::unordered_set<Entity>& stage, int focusGroup,
                   const BoneFn& bones, Snapshot& out,
                   const ChainPhysicsSystem::DebugSnapshot* dbg,
                   const ChainPhysicsSystem::DebugSnapshot* ghost)
{
    out = Snapshot{};
    if (chain == NullEntity || !world.IsAlive(chain) || !bones) return false;
    const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(chain);
    if (!skc || skc->assetIndex == kInvalidSkeletonIndex) return false;
    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (skc->assetIndex >= reg.Count()) return false;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);

    std::vector<XMFLOAT4X4> b2m;
    XMMATRIX ent = XMMatrixIdentity();
    if (!bones(renderer, world, chain, b2m, ent)) return false;
    const std::uint32_t n = std::min<std::uint32_t>(skel.boneCount,
                                                    static_cast<std::uint32_t>(b2m.size()));
    if (n == 0) return false;

    out.bone.resize(n);
    out.parent.resize(n);
    out.kind.assign(n, 0);
    out.group.assign(n, 0xFF);
    out.linkToParent.assign(n, 0);
    out.restPosed.assign(n, 0);
    for (std::uint32_t b = 0; b < n; ++b)
    {
        out.bone[b]   = BonePos(b2m, b, ent);
        out.parent[b] = skel.parentIndex[b];
    }

    // ---- the focused group (the limit-cone owner) ----
    const int groupCount = static_cast<int>(cfg.groups.size());
    auto limitOf = [&](const ChainGroupDef& g)
    {
        return g.ovrEnabled ? g.ovrLimitAngle
             : (g.type == ChainGroupType::Cloth ? cfg.skirtLimitAngle : cfg.limitAngle);
    };
    int focus = (focusGroup >= 0 && focusGroup < groupCount) ? focusGroup : -1;
    if (focus < 0 && focusGroup < 0)
        for (int gi = 0; gi < groupCount; ++gi)
        {
            const ChainGroupDef& g = cfg.groups[static_cast<std::size_t>(gi)];
            const float lim = limitOf(g);
            if (g.enabled && g.type != ChainGroupType::Spring && lim > 0.f && lim < 180.f)
            { focus = gi; break; }
        }
    out.focusGroup = focus;

    // ---- classification: InitEntity's root + subtree - excludes ----
    for (int gi = 0; gi < groupCount; ++gi)
    {
        const ChainGroupDef& g = cfg.groups[static_cast<std::size_t>(gi)];
        if (!g.enabled) continue;
        const std::uint32_t root = ResolveBone(skel, g.rootBone);
        if (root >= n) continue;
        if (out.kind[root] == 0 || out.kind[root] == 3)
        {
            out.kind[root]  = 1;
            out.group[root] = static_cast<std::uint8_t>(std::min(gi, 0xFE));
        }
        if (g.type == ChainGroupType::Spring) continue;   // one jiggle bone, no chain

        std::vector<std::uint8_t> mk(n, 0), ex(n, 0);
        mk[root] = 1;
        for (std::uint32_t b = root + 1; b < n; ++b)
            if (skel.parentIndex[b] >= 0 && static_cast<std::uint32_t>(skel.parentIndex[b]) < n &&
                mk[static_cast<std::uint32_t>(skel.parentIndex[b])])
                mk[b] = 1;
        for (int j = 0; j < g.excludeCount && j < ChainGroupDef::MAX_EXCLUDE; ++j)
        {
            const std::uint32_t exb = ResolveBone(skel, g.excludeBones[j]);
            if (exb < n) ex[exb] = 1;
        }
        if (g.excludeSubtree)
            for (std::uint32_t b = 0; b < n; ++b)
                if (skel.parentIndex[b] >= 0 && static_cast<std::uint32_t>(skel.parentIndex[b]) < n &&
                    ex[static_cast<std::uint32_t>(skel.parentIndex[b])])
                    ex[b] = 1;
        for (std::uint32_t b = 0; b < n; ++b)
        {
            if (!mk[b] || b == root) continue;
            if (ex[b]) { if (out.kind[b] == 0) out.kind[b] = 3; continue; }
            out.kind[b]  = 2;
            out.group[b] = static_cast<std::uint8_t>(std::min(gi, 0xFE));
            const std::int32_t par = skel.parentIndex[b];
            if (par >= 0 && static_cast<std::uint32_t>(par) < n && mk[par] && !ex[par])
                out.linkToParent[b] = 1;
        }

        // ---- the focused group's rest-limit cones (EmitChainPhysicsOverlay's) ----
        if (gi != focus) continue;
        const float limDeg = limitOf(g);
        if (limDeg <= 0.f || limDeg >= 180.f) continue;
        const float tipScale = g.ovrEnabled ? g.ovrLimitTipScale
            : (g.type == ChainGroupType::Cloth ? cfg.skirtLimitTipScale : cfg.limitTipScale);
        std::vector<int> depth(n, 0), leafDepth(n, 0);
        for (std::uint32_t b = root + 1; b < n; ++b)
            if (mk[b] && !ex[b] && skel.parentIndex[b] >= 0)
                depth[b] = depth[static_cast<std::uint32_t>(skel.parentIndex[b])] + 1;
        for (std::uint32_t b = 0; b < n; ++b) leafDepth[b] = depth[b];
        for (std::uint32_t b = n; b-- > root + 1; )
        {
            if (!mk[b] || ex[b] || skel.parentIndex[b] < 0) continue;
            const std::uint32_t par = static_cast<std::uint32_t>(skel.parentIndex[b]);
            if (leafDepth[b] > leafDepth[par]) leafDepth[par] = leafDepth[b];
        }
        for (std::uint32_t c = root + 1; c < n; ++c)
        {
            if (!mk[c] || ex[c] || skel.parentIndex[c] < 0) continue;
            const std::uint32_t par = static_cast<std::uint32_t>(skel.parentIndex[c]);
            if (!mk[par] || ex[par]) continue;
            const XMFLOAT3 ap = out.bone[par];
            const XMFLOAT3 cp = out.bone[c];
            XMVECTOR axis = XMVectorSet(cp.x - ap.x, cp.y - ap.y, cp.z - ap.z, 0.f);
            const float L = XMVectorGetX(XMVector3Length(axis));
            if (L < 1e-5f) continue;
            axis = XMVectorScale(axis, 1.f / L);
            const float t = (leafDepth[c] > 0)
                ? static_cast<float>(depth[c]) / static_cast<float>(leafDepth[c]) : 0.f;
            float half = limDeg * (1.f + t * (tipScale - 1.f));
            if (half <= 0.f) continue;
            if (half > 179.f) half = 179.f;
            float sinH, cosH;
            XMScalarSinCos(&sinH, &cosH, half * 0.01745329252f);
            const XMVECTOR ref = (std::fabs(XMVectorGetY(axis)) < 0.99f)
                ? XMVectorSet(0.f, 1.f, 0.f, 0.f) : XMVectorSet(1.f, 0.f, 0.f, 0.f);
            XMVECTOR u = XMVector3Cross(axis, ref);
            if (XMVectorGetX(XMVector3LengthSq(u)) < 1e-10f) continue;
            u = XMVector3Normalize(u);
            const XMVECTOR v = XMVector3Cross(axis, u);
            const XMVECTOR apex   = XMVectorSet(ap.x, ap.y, ap.z, 0.f);
            const XMVECTOR center = XMVectorAdd(apex, XMVectorScale(axis, L * cosH));
            const float    radius = L * sinH;
            constexpr int kSeg = 12;
            XMFLOAT3 prev{};
            for (int s = 0; s <= kSeg; ++s)
            {
                float sa, ca;
                XMScalarSinCos(&sa, &ca, 6.2831853f * static_cast<float>(s) / kSeg);
                XMFLOAT3 pt;
                XMStoreFloat3(&pt, XMVectorAdd(center,
                    XMVectorAdd(XMVectorScale(u, radius * ca), XMVectorScale(v, radius * sa))));
                if (s > 0) out.cone.push_back({ prev, pt });
                if (s % (kSeg / 4) == 0 && s < kSeg) out.cone.push_back({ ap, pt });
                prev = pt;
            }
        }
    }

    // ---- authored rest pose ----
    if (cfg.restPoseEnabled)
        for (const BoneRestPose& r : cfg.restPose)
        {
            if (!r.enabled) continue;
            const std::uint32_t b = ResolveBone(skel, r.boneName);
            if (b < n) out.restPosed[b] = 1;
        }

    // ---- the chain solver's capsules (CapsuleColliderComponent) ----
    // prepareEntity's shape: endpoint = bone position + offset in the bone's
    // basis, in MODEL space, then the entity transform; world radius =
    // def.radius x entity scale - bone scale deliberately NOT applied (the sim
    // never applies it to the radius; Renderer_Scene's capsule debug draw is
    // the same rule). Pool-driven over the stage set.
    if (auto* cp = world.GetPool<CapsuleColliderComponent>())
    {
        auto& ents = cp->Entities();
        auto& data = cp->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (!stage.count(e) || data[i].count <= 0) continue;
            Entity skelEnt = e;
            if (!world.HasComponent<SkeletonComponent>(e))
            {
                const SkeletonRef* ref = world.GetComponent<SkeletonRef>(e);
                if (!ref || ref->entity == NullEntity) continue;
                skelEnt = ref->entity;
            }
            const SkeletonComponent* sk2 = world.GetComponent<SkeletonComponent>(skelEnt);
            if (!sk2 || sk2->assetIndex == kInvalidSkeletonIndex || sk2->assetIndex >= reg.Count())
                continue;
            const SkeletonAsset& skel2 = reg.Get(sk2->assetIndex);
            std::vector<XMFLOAT4X4> local;
            XMMATRIX ent2 = ent;
            const std::vector<XMFLOAT4X4>* mats = &b2m;
            if (skelEnt != chain)
            {
                if (!bones(renderer, world, skelEnt, local, ent2)) continue;
                mats = &local;
            }
            const float scale = EntityScale(ent2);
            const CapsuleColliderComponent& cc = data[i];
            for (int c = 0; c < cc.count && c < CapsuleColliderComponent::MAX_CAPSULES; ++c)
            {
                const auto& def = cc.capsules[c];
                if (!def.enabled) continue;
                if (def.boneA >= skel2.boneCount || def.boneB >= skel2.boneCount ||
                    def.boneA >= mats->size() || def.boneB >= mats->size())
                    continue;
                const XMMATRIX wA = XMLoadFloat4x4(&(*mats)[def.boneA]);
                const XMMATRIX wB = XMLoadFloat4x4(&(*mats)[def.boneB]);
                const XMVECTOR pA = XMVectorAdd(wA.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetA), wA));
                const XMVECTOR pB = XMVectorAdd(wB.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetB), wB));
                Snapshot::Capsule cap{};
                XMStoreFloat3(&cap.a, XMVector3TransformCoord(XMVectorSetW(pA, 1.f), ent2));
                XMStoreFloat3(&cap.b, XMVector3TransformCoord(XMVectorSetW(pB, 1.f), ent2));
                cap.radius = def.radius * scale;
                out.capsules.push_back(cap);
            }
        }
    }

    // ---- the solver's own state ----
    // FillDebugSnapshot's positions are in SKELETON space (the solver simulates
    // there); `ent` is the SAME skeleton -> world transform the bones above
    // use, so a particle lands exactly on the bone it tracks.
    if (dbg && !dbg->particlePos.empty())
    {
        Snapshot::Sim& s = out.sim;
        const std::size_t np = dbg->particlePos.size();
        const float scale = EntityScale(ent);
        auto toWorld = [&](const XMFLOAT3& p)
        {
            XMFLOAT3 w;
            XMStoreFloat3(&w, XMVector3TransformCoord(XMVectorSet(p.x, p.y, p.z, 1.f), ent));
            return w;
        };
        s.pos.resize(np);
        s.radius.resize(np);
        s.owner.assign(np, 0);
        for (std::size_t i = 0; i < np; ++i)
        {
            s.pos[i]    = toWorld(dbg->particlePos[i]);
            s.radius[i] = dbg->particleRadius[i] * scale;
            // A writer is never a strand's tip, so i + 1 is its own strand's
            // next particle: the segment whose direction WriteBonePoses turns
            // into the bone's rotation.
            s.owner[i]  = (dbg->particleWritesBone[i] != ~0u && i + 1 < np) ? 1 : 0;
        }
        s.island = dbg->particleIsland;
        s.limit  = dbg->particleLimitViolated;
        s.pinned = dbg->particlePinned;
        s.frozen = dbg->particleFrozen;   // frozen at rest by a solo
        s.links  = dbg->links;
        s.bucket = dbg->linkBucket;
        // LOD-skipped collide groups: mark each such group's pinned roots.
        if (dbg->lodTier >= 2)
            for (std::size_t i = 0; i < np; ++i)
            {
                const std::uint8_t g = dbg->particleGroup[i];
                if (!dbg->particlePinned[i] || g >= cfg.groups.size()) continue;
                if (cfg.groups[g].enabled && cfg.groups[g].collide) s.lodSkipRoots.push_back(s.pos[i]);
            }
        for (const auto& c : dbg->capsules)
            if (c.interpenetrating)
                s.interCaps.push_back({ toWorld(c.a), toWorld(c.b), c.radius * scale });
        s.valid = true;
    }
    // ---- the A/B ghost ----
    // Parameter set B is solved on A's own pose, in A's skeleton space (the
    // pose feed), so A's skeleton -> world transform places it over A.
    if (ghost && !ghost->particlePos.empty())
    {
        Snapshot::Ghost& g = out.ghost;
        g.pos.resize(ghost->particlePos.size());
        for (std::size_t i = 0; i < ghost->particlePos.size(); ++i)
        {
            const XMFLOAT3& p = ghost->particlePos[i];
            XMStoreFloat3(&g.pos[i], XMVector3TransformCoord(XMVectorSet(p.x, p.y, p.z, 1.f), ent));
        }
        g.links = ghost->links;
        g.valid = true;
    }
    out.valid = true;
    return true;
}

// The two factors separately: ImGuizmo takes the view and the projection
// apart, and the panel's in-picture gizmo must use EXACTLY the overlay's
// matrices (one camera, one convention).
void ViewAndProj(const Camera& cam, float aspect, bool badProj, XMMATRIX& outView,
                 XMMATRIX& outProj)
{
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&cam.forward));
    XMVECTOR up  = XMVector3Normalize(XMLoadFloat3(&cam.up));
    if (XMVector3IsNaN(up) || std::fabs(XMVectorGetX(XMVector3Dot(up, fwd))) > 0.999f)
        up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    if (badProj) fwd = XMVectorNegate(fwd);   // reverse arm: look away
    const float nearZ = std::max(cam.nearZ, 1e-4f);
    const float farZ  = std::max(cam.farZ, nearZ + 1e-3f);
    outView = XMMatrixLookToLH(XMLoadFloat3(&cam.eye), fwd, up);
    // Reverse-Z: FAR first, then NEAR - ExtractPreviewCameras' convention.
    outProj = XMMatrixPerspectiveFovLH(cam.fovY, aspect > 0.f ? aspect : 1.f, farZ, nearZ);
}

XMMATRIX ViewProj(const Camera& cam, float aspect, bool badProj)
{
    XMMATRIX view, proj;
    ViewAndProj(cam, aspect, badProj, view, proj);
    return XMMatrixMultiply(view, proj);
}

// ==== UI half: project, painter-sort, emit ====
namespace
{
    // The stats box, lower-left. Line 0 - the solve cost - is the prominent one
    // (the knobs the panel exposes multiply it: nSub x (1 + iterations) collision
    // passes per island).
    std::uint32_t DrawStatsBox(ImDrawList* dl, const ImVec2& rectMin, const ImVec2& rectSize,
                               const StatsBox& box)
    {
        if (!box.show || box.lines.empty() || !ImGui::GetCurrentContext()) return 0;
        ImFont* font = ImGui::GetFont();
        const float base = ImGui::GetFontSize();
        const float big  = base * 1.35f;
        float w = 0.f, h = 0.f;
        for (std::size_t i = 0; i < box.lines.size(); ++i)
        {
            const float fs = i == 0 ? big : base;
            const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0.f, box.lines[i].c_str());
            w = std::max(w, sz.x);
            h += fs + 2.f;
        }
        const float pad = 6.f;
        const ImVec2 p0(rectMin.x + 8.f, rectMin.y + rectSize.y - 8.f - h - 2.f * pad);
        const ImVec2 p1(p0.x + w + 2.f * pad, p0.y + h + 2.f * pad);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 170), 4.f);
        float y = p0.y + pad;
        for (std::size_t i = 0; i < box.lines.size(); ++i)
        {
            const float fs = i == 0 ? big : base;
            dl->AddText(font, fs, ImVec2(p0.x + pad, y),
                        i == 0 ? IM_COL32(255, 220, 90, 255) : IM_COL32(230, 230, 230, 255),
                        box.lines[i].c_str());
            y += fs + 2.f;
        }
        return static_cast<std::uint32_t>(box.lines.size());
    }

    ImU32 IslandColour(std::uint8_t k)
    {
        static const ImU32 kPal[8] = {
            IM_COL32(255, 99, 71, 255),  IM_COL32(60, 179, 113, 255), IM_COL32(65, 105, 225, 255),
            IM_COL32(255, 215, 0, 255),  IM_COL32(186, 85, 211, 255), IM_COL32(0, 206, 209, 255),
            IM_COL32(255, 140, 0, 255),  IM_COL32(154, 205, 50, 255) };
        return kPal[k & 7u];
    }
}

Stats Draw(ImDrawList* dl, const ImVec2& rectMin, const ImVec2& rectSize,
           const XMMATRIX& viewProj, float fovY, const Snapshot& snap,
           std::uint32_t mask, bool depthFade, const StatsBox* stats)
{
    Stats st{};
    // The corner read-out, FIRST in the list and outside the painter sort (the
    // overlay's primitives draw over it only where they cross that corner).
    if (dl && stats && rectSize.x > 0.f && rectSize.y > 0.f)
        st.statsLines = DrawStatsBox(dl, rectMin, rectSize, *stats);
    if (!dl || !snap.valid || mask == 0 || !(rectSize.x > 0.f) || !(rectSize.y > 0.f))
        return st;
    const float W = rectSize.x, H = rectSize.y;
    const float focalPx = 0.5f * H / std::tan(0.5f * std::max(fovY, 1e-3f));

    struct Proj { float x = 0.f, y = 0.f, w = 0.f; bool ok = false, inView = false; };
    auto project = [&](const XMFLOAT3& p)
    {
        Proj r;
        const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.f), viewProj);
        r.w = XMVectorGetW(c);
        if (!(r.w > 1e-6f) || !std::isfinite(r.w)) return r;
        const float nx = XMVectorGetX(c) / r.w, ny = XMVectorGetY(c) / r.w,
                    nz = XMVectorGetZ(c) / r.w;
        r.ok = std::isfinite(nx) && std::isfinite(ny);
        r.inView = r.ok && std::fabs(nx) <= 1.f && std::fabs(ny) <= 1.f && nz >= 0.f && nz <= 1.f;
        r.x = rectMin.x + (nx * 0.5f + 0.5f) * W;
        r.y = rectMin.y + (1.f - (ny * 0.5f + 0.5f)) * H;
        return r;
    };

    enum class T : std::uint8_t { Disc, Ring, Line, Cross };
    struct Prim { T t; ImVec2 a, b; float r, thick, depth; ImU32 col; };
    std::vector<Prim> prims;
    prims.reserve(snap.bone.size() * 2 + snap.cone.size() + snap.capsules.size() * 4);

    const std::uint32_t n = static_cast<std::uint32_t>(snap.bone.size());
    std::vector<Proj> pj(n);
    for (std::uint32_t b = 0; b < n; ++b)
        if (snap.kind[b] != 0 || snap.restPosed[b]) pj[b] = project(snap.bone[b]);

    const int focus = snap.focusGroup;
    auto isFocus = [&](std::uint32_t b)
    { return focus >= 0 && snap.group[b] == static_cast<std::uint8_t>(focus); };

    for (std::uint32_t b = 0; b < n; ++b)
    {
        const std::uint8_t k = snap.kind[b];
        const bool want = (k == 1 && (mask & kRoots)) || (k == 2 && (mask & kChain)) ||
                          (k == 3 && (mask & kExcludes));
        if (want)
        {
            ++st.bones;
            if (k == 3) ++st.excludes;
            if (!pj[b].inView) ++st.offscreenBones;
            if (pj[b].ok)
            {
                const ImVec2 p(pj[b].x, pj[b].y);
                const bool hi = isFocus(b);
                if (k == 1)
                    prims.push_back({ T::Disc, p, p, hi ? 6.5f : 5.f, 0.f, pj[b].w,
                                      hi ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 208, 0, 255) });
                else if (k == 2)
                    prims.push_back({ T::Disc, p, p, hi ? 3.5f : 3.f, 0.f, pj[b].w,
                                      hi ? IM_COL32(255, 122, 0, 255) : IM_COL32(0, 224, 255, 255) });
                else
                    prims.push_back({ T::Cross, p, p, 4.f, 1.5f, pj[b].w, IM_COL32(255, 48, 48, 255) });
            }
        }
        if ((mask & kLinks) && k == 2 && snap.linkToParent[b])
        {
            const std::int32_t par = snap.parent[b];
            if (par >= 0 && static_cast<std::uint32_t>(par) < n)
            {
                const Proj& a = pj[static_cast<std::uint32_t>(par)];
                if (a.ok && pj[b].ok)
                {
                    ++st.links;
                    const bool hi = isFocus(b);
                    prims.push_back({ T::Line, ImVec2(a.x, a.y), ImVec2(pj[b].x, pj[b].y), 0.f,
                                      1.5f, 0.5f * (a.w + pj[b].w),
                                      hi ? IM_COL32(255, 176, 0, 255) : IM_COL32(64, 255, 64, 255) });
                }
            }
        }
        if ((mask & kRestPose) && snap.restPosed[b] && pj[b].ok)
        {
            ++st.restPosed;
            prims.push_back({ T::Ring, ImVec2(pj[b].x, pj[b].y), ImVec2(), 7.f, 1.5f,
                              pj[b].w, IM_COL32(255, 64, 255, 255) });
        }
    }
    if (mask & kCone)
        for (const auto& seg : snap.cone)
        {
            const Proj a = project(seg.first), c = project(seg.second);
            if (!a.ok || !c.ok) continue;
            ++st.coneSegs;
            prims.push_back({ T::Line, ImVec2(a.x, a.y), ImVec2(c.x, c.y), 0.f, 1.f,
                              0.5f * (a.w + c.w), IM_COL32(176, 96, 255, 255) });
        }
    if (mask & kCapsules)
        for (const Snapshot::Capsule& cap : snap.capsules)
        {
            const Proj a = project(cap.a), c = project(cap.b);
            if (!a.ok || !c.ok) continue;
            ++st.capsules;
            const float ra = cap.radius * focalPx / a.w, rc = cap.radius * focalPx / c.w;
            const ImU32 col = IM_COL32(235, 235, 235, 220);
            const float d = 0.5f * (a.w + c.w);
            prims.push_back({ T::Ring, ImVec2(a.x, a.y), ImVec2(), ra, 1.f, a.w, col });
            prims.push_back({ T::Ring, ImVec2(c.x, c.y), ImVec2(), rc, 1.f, c.w, col });
            const float dx = c.x - a.x, dy = c.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.5f)
            {
                const float nx = -dy / len, ny = dx / len;
                prims.push_back({ T::Line, ImVec2(a.x + nx * ra, a.y + ny * ra),
                                  ImVec2(c.x + nx * rc, c.y + ny * rc), 0.f, 1.f, d, col });
                prims.push_back({ T::Line, ImVec2(a.x - nx * ra, a.y - ny * ra),
                                  ImVec2(c.x - nx * rc, c.y - ny * rc), 0.f, 1.f, d, col });
            }
        }

    // ---- the solver's own state ----
    const Snapshot::Sim& sim = snap.sim;
    if (sim.valid && (mask & kSimBits))
    {
        const std::size_t np = sim.pos.size();
        std::vector<Proj> pp(np);
        for (std::size_t i = 0; i < np; ++i) pp[i] = project(sim.pos[i]);
        if (mask & kConstraints)
            for (std::size_t k = 0; k < sim.links.size(); ++k)
            {
                const std::uint32_t a = sim.links[k].first, b = sim.links[k].second;
                if (a >= np || b >= np || !pp[a].ok || !pp[b].ok) continue;
                ++st.constraints;
                const std::uint8_t bk = k < sim.bucket.size() ? sim.bucket[k] : 0;
                // vertical (0-1) blue, ring (2-3) yellow, shear (4-5) magenta
                const ImU32 col = bk < 2 ? IM_COL32(80, 170, 255, 230)
                                : bk < 4 ? IM_COL32(255, 225, 64, 230)
                                         : IM_COL32(255, 90, 200, 200);
                prims.push_back({ T::Line, ImVec2(pp[a].x, pp[a].y), ImVec2(pp[b].x, pp[b].y), 0.f,
                                  bk < 2 ? 1.5f : 1.f, 0.5f * (pp[a].w + pp[b].w), col });
            }
        if (mask & kOwnership)
            for (std::size_t i = 0; i + 1 < np; ++i)
            {
                if (!sim.owner[i] || !pp[i].ok || !pp[i + 1].ok) continue;
                ++st.ownerLines;
                prims.push_back({ T::Line, ImVec2(pp[i].x, pp[i].y), ImVec2(pp[i + 1].x, pp[i + 1].y),
                                  0.f, 3.f, 0.5f * (pp[i].w + pp[i + 1].w), IM_COL32(255, 255, 255, 170) });
            }
        if (mask & (kParticles | kIslands))
            for (std::size_t i = 0; i < np; ++i)
            {
                if (!pp[i].ok) continue;
                ++st.particles;
                // A particle FROZEN AT REST by a solo is slate grey (it is pinned too, but
                // it is not a root).
                const bool frozen = i < sim.frozen.size() && sim.frozen[i] != 0;
                if (frozen) ++st.frozenMarks;
                const ImU32 col = (mask & kIslands) ? IslandColour(sim.island[i])
                                : frozen ? IM_COL32(120, 140, 170, 255)
                                : sim.pinned[i] ? IM_COL32(255, 208, 0, 255) : IM_COL32(0, 255, 160, 255);
                const ImVec2 c(pp[i].x, pp[i].y);
                prims.push_back({ T::Disc, c, c, 2.5f, 0.f, pp[i].w, col });
                const float rpx = sim.radius[i] * focalPx / pp[i].w;
                if ((mask & kParticles) && rpx >= 1.5f)
                    prims.push_back({ T::Ring, c, ImVec2(), rpx, 1.f, pp[i].w,
                                      IM_COL32(0, 255, 160, 140) });
            }
        if (mask & kLimits)
            for (std::size_t i = 0; i < np; ++i)
            {
                if (!sim.limit[i] || !pp[i].ok) continue;
                ++st.limitMarks;
                const ImVec2 c(pp[i].x, pp[i].y);
                if (sim.limit[i] >= 2)
                    prims.push_back({ T::Disc, c, c, 4.5f, 0.f, pp[i].w, IM_COL32(255, 30, 30, 255) });
                else
                    prims.push_back({ T::Ring, c, ImVec2(), 5.5f, 2.f, pp[i].w, IM_COL32(255, 40, 40, 255) });
            }
        if (mask & kLodSkip)
            for (const XMFLOAT3& r : sim.lodSkipRoots)
            {
                const Proj a = project(r);
                if (!a.ok) continue;
                ++st.lodMarks;
                prims.push_back({ T::Cross, ImVec2(a.x, a.y), ImVec2(), 6.f, 2.f, a.w,
                                  IM_COL32(160, 160, 160, 255) });
            }
        if (mask & kInterpen)
            for (const Snapshot::Capsule& cap : sim.interCaps)
            {
                const Proj a = project(cap.a), c = project(cap.b);
                if (!a.ok || !c.ok) continue;
                ++st.interCaps;
                const float ra = cap.radius * focalPx / a.w, rc = cap.radius * focalPx / c.w;
                const ImU32 col = IM_COL32(255, 40, 40, 235);
                prims.push_back({ T::Ring, ImVec2(a.x, a.y), ImVec2(), ra, 2.f, a.w, col });
                prims.push_back({ T::Ring, ImVec2(c.x, c.y), ImVec2(), rc, 2.f, c.w, col });
                prims.push_back({ T::Line, ImVec2(a.x, a.y), ImVec2(c.x, c.y), 0.f, 2.f,
                                  0.5f * (a.w + c.w), col });
            }
    }

    // ---- the A/B ghost: parameter set B, desaturated ----
    // Links as thin grey lines and particles as small grey dots, in the same
    // depth sort as everything else: where B agrees with A the ghost hides
    // under A's own primitives, where it diverges it separates from them.
    if ((mask & kGhost) && snap.ghost.valid)
    {
        const std::size_t ng = snap.ghost.pos.size();
        std::vector<Proj> gp(ng);
        for (std::size_t i = 0; i < ng; ++i) gp[i] = project(snap.ghost.pos[i]);
        for (const auto& l : snap.ghost.links)
        {
            if (l.first >= ng || l.second >= ng || !gp[l.first].ok || !gp[l.second].ok) continue;
            ++st.ghostLinks;
            prims.push_back({ T::Line, ImVec2(gp[l.first].x, gp[l.first].y),
                              ImVec2(gp[l.second].x, gp[l.second].y), 0.f, 1.f,
                              0.5f * (gp[l.first].w + gp[l.second].w), IM_COL32(200, 200, 205, 150) });
        }
        for (std::size_t i = 0; i < ng; ++i)
        {
            if (!gp[i].ok) continue;
            ++st.ghostParticles;
            const ImVec2 c(gp[i].x, gp[i].y);
            prims.push_back({ T::Disc, c, c, 1.8f, 0.f, gp[i].w, IM_COL32(215, 215, 220, 170) });
        }
    }

    // ---- painter's order + distance fade (the stand-in for occlusion) ----
    float dmin = 1e30f, dmax = -1e30f;
    for (const Prim& p : prims) { dmin = std::min(dmin, p.depth); dmax = std::max(dmax, p.depth); }
    std::stable_sort(prims.begin(), prims.end(),
                     [](const Prim& x, const Prim& y) { return x.depth > y.depth; });
    const float span = (dmax > dmin) ? (dmax - dmin) : 1.f;
    for (const Prim& p : prims)
    {
        ImU32 col = p.col;
        if (depthFade)
        {
            const float t = std::clamp((p.depth - dmin) / span, 0.f, 1.f);
            const float a = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFFu) * (1.f - 0.65f * t);
            col = (col & ~IM_COL32_A_MASK) |
                  (static_cast<ImU32>(std::clamp(a, 0.f, 255.f)) << IM_COL32_A_SHIFT);
        }
        switch (p.t)
        {
        case T::Disc:
            dl->AddCircleFilled(p.a, p.r, col, 12);
            dl->AddCircle(p.a, p.r, IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFFu), 12, 1.f);
            break;
        case T::Ring:
            if (p.r >= 0.5f && p.r < 4096.f) dl->AddCircle(p.a, p.r, col, 16, p.thick);
            break;
        case T::Line:
            dl->AddLine(p.a, p.b, col, p.thick);
            break;
        case T::Cross:
            dl->AddLine(ImVec2(p.a.x - p.r, p.a.y - p.r), ImVec2(p.a.x + p.r, p.a.y + p.r), col, p.thick);
            dl->AddLine(ImVec2(p.a.x - p.r, p.a.y + p.r), ImVec2(p.a.x + p.r, p.a.y - p.r), col, p.thick);
            break;
        }
    }
    st.prims = static_cast<std::uint32_t>(prims.size());
    return st;
}

// ==== Harness: the overlay layer as a PNG (software raster of a private draw list) ====
long long WriteLayerPng(const std::string& path, std::uint32_t w, std::uint32_t h,
                        const XMMATRIX& viewProj, float fovY, const Snapshot& snap,
                        std::uint32_t mask, bool depthFade, Stats* outStats,
                        std::uint32_t* outTris, std::string* err, const StatsBox* stats)
{
    auto fail = [&](const char* why) { if (err) *err = why; return -1ll; };
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return fail("bad size");
    if (!ImGui::GetCurrentContext()) return fail("no ImGui context");

    // A PRIVATE list: the same Draw() call the panel makes, at the output's own
    // pixel rect. Geometric anti-aliasing (no texture-lookup lines), so every
    // pixel of the layer is a function of the vertex colours alone.
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    dl.PushClipRect(ImVec2(0.f, 0.f), ImVec2(static_cast<float>(w), static_cast<float>(h)), false);
    const Stats st = Draw(&dl, ImVec2(0.f, 0.f),
                          ImVec2(static_cast<float>(w), static_cast<float>(h)),
                          viewProj, fovY, snap, mask, depthFade, stats);
    if (outStats) *outStats = st;

    // The raster is shared with the Mesh Editor's shots (DrawListRaster): a
    // change to it changes this layer's golden.
    return RasterizeDrawListToPng(dl, w, h, path, outTris, err);
}

} // namespace ChainOverlay
} // namespace Editor
