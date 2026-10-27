#include "PostProcess/PostProcessResolve.h"
#include "PostProcess/PostProcessRuntime.h"
#include "PostProcess/ProfileSystem.h"
#include "System/CommandLine.h"

#include "ECS/ECS.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace DirectX;

namespace PostProcess
{

float DistanceToSurface(const XMFLOAT4X4& m, bool isBox, const XMFLOAT3& p)
{
    const XMMATRIX M = XMLoadFloat4x4(&m);

    XMVECTOR scale, rotQ, trans;
    if (!XMMatrixDecompose(&scale, &rotQ, &trans, M))
    {
        // Degenerate matrix (zero scale etc.): fall back to its translation.
        trans = M.r[3];
        scale = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
        rotQ  = XMQuaternionIdentity();
    }

    const XMVECTOR P = XMLoadFloat3(&p);
    const XMVECTOR d = XMVectorSubtract(P, trans);

    if (!isBox)
    {
        // Sphere: radius = scale.x. The distance is rotation-invariant.
        const float radius = XMVectorGetX(scale);
        return XMVectorGetX(XMVector3Length(d)) - radius;
    }

    // OBB: rotate the offset into the box's local frame, then evaluate an AABB SDF
    // with half-extents = scale. The rotation is orthonormal, so the result stays in
    // world units (as blendDistance is).
    const XMVECTOR localD = XMVector3Rotate(d, XMQuaternionInverse(rotQ));
    XMFLOAT3 ld;   XMStoreFloat3(&ld, localD);
    XMFLOAT3 half; XMStoreFloat3(&half, scale);

    const float qx = std::fabs(ld.x) - half.x;
    const float qy = std::fabs(ld.y) - half.y;
    const float qz = std::fabs(ld.z) - half.z;

    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    const float oz = std::max(qz, 0.0f);
    const float outside = std::sqrt(ox * ox + oy * oy + oz * oz);
    const float inside  = std::min(std::max(qx, std::max(qy, qz)), 0.0f);
    return outside + inside;
}

namespace
{
    float WeightForVolume(const ECS::PostProcessVolumeComponent& v, float signedDist)
    {
        if (v.isGlobal)            return v.blendWeight;
        if (signedDist <= 0.0f)    return v.blendWeight;          // inside
        if (v.blendDistance <= 0.0f) return 0.0f;                 // hard edge, outside
        if (signedDist >= v.blendDistance) return 0.0f;
        const float falloff = 1.0f - (signedDist / v.blendDistance);
        return v.blendWeight * falloff;
    }

    void CopyLabel(char (&dst)[64], const std::string& path, bool isGlobal)
    {
        if (isGlobal && path.empty()) { std::snprintf(dst, 64, "%s", "(global)"); return; }
        // The basename of the profile path, else a placeholder.
        std::string base = path;
        const auto slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base.empty()) base = "(volume)";
        std::snprintf(dst, 64, "%s", base.c_str());
    }
}

void ResolveAt(World& world, const XMFLOAT3& cameraPos, uint32_t viewLayerMask,
               ResolvedPostProcessSettings& out, std::vector<VolumeHitDebug>* hits)
{
    Runtime& rt = Runtime::Get();
    bool linearOnly = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -pp_blend_linear: every float blends linearly, the exposure multipliers and the hue shift too
    // (a reverse arm).
    static const bool s_linear = Sys::CommandLine::Get().Has("-pp_blend_linear");
    linearOnly = s_linear;
#endif

    // 1. The engine default is the base of the blend.
    ResolvedPostProcessSettings result = Flatten(ProfileSystem::Get().EngineDefault());

    // 2-3. Gather candidate volumes and their effective weights.
    struct Candidate
    {
        const PostProcessProfile* profile;
        float    weight;
        float    priority;
        uint32_t order;          // stable tiebreak (iteration order)
        char     label[64];
        bool     isGlobal;
    };
    std::vector<Candidate> candidates;

    uint32_t order = 0;
    world.ForEach<ECS::PostProcessVolumeComponent>(
        [&](Entity e, ECS::PostProcessVolumeComponent& v)
    {
        const uint32_t myOrder = order++;
        if ((v.layerMask & viewLayerMask) == 0) return;

        const PostProcessProfile* p = ProfileSystem::Get().Get(v.profile);
        if (!p || !p->HasAnyOverride()) return;

        float weight;
        if (v.isGlobal)
        {
            weight = v.blendWeight;
        }
        else
        {
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) return;   // bounded volume needs a transform
            const float sd = DistanceToSurface(gt->matrix,
                                               v.shape == ECS::PPVolumeShape::Box,
                                               cameraPos);
            weight = WeightForVolume(v, sd);
        }
        if (weight <= 0.0f) return;

        Candidate c{};
        c.profile  = p;
        c.weight   = weight;
        c.priority = v.priority;
        c.order    = myOrder;
        c.isGlobal = v.isGlobal;
        CopyLabel(c.label, v.profilePath, v.isGlobal);
        candidates.push_back(c);
    });

    // 3. Priority ascending; ties broken by iteration order (stable).
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.order < b.order;
        });

    // 4. Sequential lerp of each volume's overridden properties.
    for (const Candidate& c : candidates)
        BlendProfileInto(result, *c.profile, c.weight, linearOnly);

    // 5. Gameplay override stack, applied on top (ResolveForView advanced and pruned it).
    std::vector<const PostProcessOverride*> ordered;
    ordered.reserve(rt.overrides.size());
    for (auto& o : rt.overrides) ordered.push_back(&o);
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const PostProcessOverride* a, const PostProcessOverride* b)
        { return a->priority < b->priority; });

    for (const PostProcessOverride* o : ordered)
    {
        if ((o->layerMask & viewLayerMask) == 0) continue;
        const float w = o->masterWeight * EvaluateOverrideEnvelope(*o);
        if (w <= 0.0f) continue;
        BlendProfileInto(result, o->profile, w, linearOnly);
    }

    out = result;

    if (hits)
    {
        hits->clear();
        hits->reserve(candidates.size());
        for (const Candidate& c : candidates)
        {
            VolumeHitDebug h{};
            std::snprintf(h.label, sizeof(h.label), "%s", c.label);
            h.weight   = c.weight;
            h.priority = c.priority;
            h.isGlobal = c.isGlobal;
            hits->push_back(h);
        }
    }
}

void ResolveForView(World& world, const XMFLOAT3& cameraPos,
                    uint32_t viewLayerMask, float dt)
{
    Runtime& rt = Runtime::Get();

    // The override stack: advance time, prune the expired.
    for (auto& o : rt.overrides) o.elapsed += dt;
    rt.overrides.erase(
        std::remove_if(rt.overrides.begin(), rt.overrides.end(),
                       [](const PostProcessOverride& o) { return IsOverrideExpired(o); }),
        rt.overrides.end());

    // The editor overlay's snapshot of which volumes counted, while it is on.
    ResolveAt(world, cameraPos, viewLayerMask, rt.resolved, rt.debugCapture ? &rt.debugHits : nullptr);
}

} // namespace PostProcess
