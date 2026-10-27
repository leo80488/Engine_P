// ChainRestPoseMath.cpp - see include/Editor/ChainRestPoseMath.h. The one
// implementation behind the main viewport's gizmo and the Chain Physics
// Preview panel's in-picture gizmo.

#include "Editor/ChainRestPoseMath.h"

#include "Physics/ChainPhysicsSystem.h"   // ChainPhysicsComponent / ChainGroupDef
#include "Resource/SkeletonAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Editor
{
namespace ChainRestPose
{
using namespace DirectX;

XMMATRIX OrthonormalizeRotation(XMMATRIX m)
{
    XMVECTOR x = XMVector3Normalize(m.r[0]);
    XMVECTOR y = XMVector3Normalize(XMVectorSubtract(
        m.r[1], XMVectorScale(x, XMVectorGetX(XMVector3Dot(m.r[1], x)))));
    XMVECTOR z = XMVector3Cross(x, y);
    XMMATRIX o;
    o.r[0] = XMVectorSetW(x, 0.f);
    o.r[1] = XMVectorSetW(y, 0.f);
    o.r[2] = XMVectorSetW(z, 0.f);
    o.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);
    return o;
}

bool ApplyWorldDelta(const XMFLOAT4& oldLocal, const XMFLOAT4X4& parentWorldIn,
                     const XMFLOAT4X4& deltaWorldIn, DeltaOrder order, XMFLOAT4& outLocal)
{
    // Parent space for the delta. Translation dropped, determinant checked, then
    // orthonormalized - see OrthonormalizeRotation.
    XMMATRIX parentWorld = XMLoadFloat4x4(&parentWorldIn);
    parentWorld.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);
    if (XMVectorGetX(XMMatrixDeterminant(parentWorld)) <= 0.f)
        return false;   // mirrored instance: a reflected frame has no rotation-only delta
    parentWorld = OrthonormalizeRotation(parentWorld);

    const XMMATRIX deltaWorld = XMLoadFloat4x4(&deltaWorldIn);

    // Row-vector convention: newLocal = oldLocal * parent * delta * parent^-1.
    const XMMATRIX deltaLocal = XMMatrixMultiply(
        XMMatrixMultiply(parentWorld, deltaWorld),
        XMMatrixInverse(nullptr, parentWorld));
    const XMVECTOR deltaRot = XMQuaternionNormalize(XMQuaternionRotationMatrix(deltaLocal));

    // Engine convention (IKSystem::ComposeIKAnim): base first, parent-frame delta
    // second. BoneLocal reverses the operands (the harness's reverse arm; see
    // DeltaOrder).
    const XMVECTOR oldRot = XMQuaternionNormalize(XMLoadFloat4(&oldLocal));
    XMVECTOR newRot = order == DeltaOrder::ParentFrame
        ? XMQuaternionNormalize(XMQuaternionMultiply(oldRot, deltaRot))
        : XMQuaternionNormalize(XMQuaternionMultiply(deltaRot, oldRot));
    // Stay in oldLocal's hemisphere (q == -q): no 360-degree flip mid-drag.
    if (XMVectorGetX(XMVector4Dot(oldRot, newRot)) < 0.f)
        newRot = XMVectorNegate(newRot);
    XMStoreFloat4(&outLocal, newRot);
    return true;
}

void CollectChainBones(const SkeletonAsset& skel, const ChainPhysicsComponent& comp,
                       int focusGroup, std::vector<std::uint8_t>& outPickable)
{
    outPickable.assign(skel.boneCount, 0);

    auto resolveBone = [&](const char* nm) -> std::uint32_t {
        if (!nm || !nm[0]) return ~0u;
        for (std::uint32_t b = 0; b < skel.boneCount; ++b)
            if (std::strcmp(skel.boneNames[b], nm) == 0) return b;
        return ~0u;
    };

    // An out-of-range focus index means "no group at all" if taken literally,
    // which silently empties the pickable set while the combo still reads
    // "All groups". Treat it as -1 so what you see is what you can click.
    if (focusGroup >= static_cast<int>(comp.groups.size())) focusGroup = -1;

    for (int gi = 0; gi < static_cast<int>(comp.groups.size()); ++gi)
    {
        if (focusGroup >= 0 && gi != focusGroup) continue;
        const ChainGroupDef& g = comp.groups[gi];
        if (!g.enabled) continue;
        const std::uint32_t root = resolveBone(g.rootBone);
        if (root >= skel.boneCount) continue;

        outPickable[root] = 1;
        // A Spring group's root IS the jiggle bone; it owns no subtree.
        if (g.type == ChainGroupType::Spring) continue;

        std::vector<std::uint8_t> mk(skel.boneCount, 0), ex(skel.boneCount, 0);
        mk[root] = 1;
        for (std::uint32_t b = root + 1; b < skel.boneCount; ++b)
            if (skel.parentIndex[b] >= 0 && mk[skel.parentIndex[b]]) mk[b] = 1;
        for (int j = 0; j < g.excludeCount; ++j) {
            const std::uint32_t exb = resolveBone(g.excludeBones[j]);
            if (exb < skel.boneCount) ex[exb] = 1;
        }
        if (g.excludeSubtree)
            for (std::uint32_t b = 0; b < skel.boneCount; ++b)
                if (skel.parentIndex[b] >= 0 && ex[skel.parentIndex[b]]) ex[b] = 1;

        for (std::uint32_t b = 0; b < skel.boneCount; ++b)
            if (mk[b] && !ex[b]) outPickable[b] = 1;
    }
}

float AngleDeg(const XMFLOAT4& a, const XMFLOAT4& b)
{
    const XMVECTOR qa = XMQuaternionNormalize(XMLoadFloat4(&a));
    const XMVECTOR qb = XMQuaternionNormalize(XMLoadFloat4(&b));
    const float d = std::min(std::fabs(XMVectorGetX(XMVector4Dot(qa, qb))), 1.f);
    return 2.f * std::acos(d) * (180.f / XM_PI);
}
}
}
