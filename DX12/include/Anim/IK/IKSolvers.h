#pragma once
#include "Anim/IK/Kinematics.h"
#include <cmath>
#include <limits>
namespace Anim::IK {
using namespace DirectX;
inline float Clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// ==== Euler angle utilities (ported from saba) ====

inline float NormalizeAngle(float angle)
{
    float ret = angle;
    while (ret >= XM_2PI) ret -= XM_2PI;
    while (ret < 0.f)    ret += XM_2PI;
    return ret;
}

inline float DiffAngle(float a, float b)
{
    float diff = NormalizeAngle(a) - NormalizeAngle(b);
    if (diff > XM_PI)       return diff - XM_2PI;
    else if (diff < -XM_PI) return diff + XM_2PI;
    return diff;
}

// Decomposes a rotation matrix into XYZ Euler angles, choosing the representation closest to before
// to prevent flipping (ported from saba, converted from column- to row-major).
// saba uses column-major glm, m[col][row]; our row-major XMFLOAT4X4 rm._RC is 1-indexed row/col:
// glm m[col][row] = M_col(row,col) -> M_row(col,row) = rm._((col+1)(row+1)).
inline XMFLOAT3 DecomposeEuler(const XMFLOAT4X4& rm, const XMFLOAT3& before)
{
    XMFLOAT3 r;

    // rm._13 corresponds to saba's m[0][2] = sin(ry)
    float sy = -rm._13;
    const float e = 1.0e-6f;

    if ((1.0f - std::fabsf(sy)) < e)
    {
        // Gimbal lock
        r.y = std::asinf(Clampf(sy, -1.f, 1.f));

        float sx = std::sinf(before.x);
        float sz = std::sinf(before.z);
        if (std::fabsf(sx) < std::fabsf(sz))
        {
            float cx = std::cosf(before.x);
            if (cx > 0.f)
            {
                r.x = 0.f;
                r.z = std::asinf(Clampf(-rm._21, -1.f, 1.f)); // saba: -m[1][0]
            }
            else
            {
                r.x = XM_PI;
                r.z = std::asinf(Clampf(rm._21, -1.f, 1.f));  // saba: m[1][0]
            }
        }
        else
        {
            float cz = std::cosf(before.z);
            if (cz > 0.f)
            {
                r.z = 0.f;
                r.x = std::asinf(Clampf(-rm._32, -1.f, 1.f)); // saba: -m[2][1]
            }
            else
            {
                r.z = XM_PI;
                r.x = std::asinf(Clampf(rm._32, -1.f, 1.f));  // saba: m[2][1]
            }
        }
    }
    else
    {
        r.x = std::atan2f(rm._23, rm._33); // saba: atan2(m[1][2], m[2][2])
        r.y = std::asinf(Clampf(-rm._13, -1.f, 1.f));  // saba: asin(-m[0][2])
        r.z = std::atan2f(rm._12, rm._11); // saba: atan2(m[0][1], m[0][0])
    }

    // Test 8 alternative Euler representations; pick the one closest to before.
    const float pi = XM_PI;
    XMFLOAT3 tests[] =
    {
        { r.x + pi,  pi - r.y, r.z + pi },
        { r.x + pi,  pi - r.y, r.z - pi },
        { r.x + pi, -pi - r.y, r.z + pi },
        { r.x + pi, -pi - r.y, r.z - pi },
        { r.x - pi,  pi - r.y, r.z + pi },
        { r.x - pi,  pi - r.y, r.z - pi },
        { r.x - pi, -pi - r.y, r.z + pi },
        { r.x - pi, -pi - r.y, r.z - pi },
    };

    float errX = std::fabsf(DiffAngle(r.x, before.x));
    float errY = std::fabsf(DiffAngle(r.y, before.y));
    float errZ = std::fabsf(DiffAngle(r.z, before.z));
    float minErr = errX + errY + errZ;

    for (const auto& t : tests)
    {
        float err = std::fabsf(DiffAngle(t.x, before.x))
                  + std::fabsf(DiffAngle(t.y, before.y))
                  + std::fabsf(DiffAngle(t.z, before.z));
        if (err < minErr)
        {
            minErr = err;
            r = t;
        }
    }

    return r;
}

// Quaternion from XYZ Euler angles, in saba's order Rz * Ry * Rx; in DirectXMath's Multiply
// convention that is Multiply(Multiply(Rx, Ry), Rz).
inline XMVECTOR QuatFromEulerXYZ(float rx, float ry, float rz)
{
    XMVECTOR qx = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), rx);
    XMVECTOR qy = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), ry);
    XMVECTOR qz = XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), rz);
    return XMQuaternionMultiply(XMQuaternionMultiply(qx, qy), qz);
}

// ==== Quaternion convention helpers ====
// saba (glm):   q1 * q2 = H(q1, q2), "apply q2 first, then q1"
// DirectXMath:  XMQuaternionMultiply(q1, q2) = H(q2, q1), "apply q1 first, then q2"
// So glm's (A * B) <-> DX Multiply(B, A):
//   finalRot = ikRot * animRot          -> DX: Multiply(animRot, ikRot)
//   chainRot = ikRot * animRot * delta  -> DX: Multiply(Multiply(delta, animRot), ikRot)
//   ikRot    = chainRot * inv(animRot)  -> DX: Multiply(conj(animRot), chainRot)

// final = ikRot after animRot (saba order: animRot first, then ikRot)
inline XMVECTOR ComposeIKAnim(XMVECTOR ikRot, XMVECTOR animRot)
{
    return XMQuaternionMultiply(animRot, ikRot);
}

// ikRot = chainRot * inv(animRot) (saba convention)
inline XMVECTOR ExtractIKRot(XMVECTOR chainRot, XMVECTOR animRot)
{
    return XMQuaternionMultiply(XMQuaternionConjugate(animRot), chainRot);
}

// chainRot = ikRot * animRot * delta (saba order)
inline XMVECTOR CombineChainRot(XMVECTOR ikRot, XMVECTOR animRot, XMVECTOR delta)
{
    return XMQuaternionMultiply(XMQuaternionMultiply(delta, animRot), ikRot);
}


class MmdCcd {
public:
    // Per-chain-link runtime state used during solving.
    struct ChainState
    {
        DirectX::XMVECTOR animRot;       // original animation rotation (before IK)
        DirectX::XMVECTOR ikRot;         // accumulated IK rotation
        DirectX::XMVECTOR saveIKRot;     // best-so-far IK rotation
        DirectX::XMFLOAT3 prevAngle;     // previous Euler angles for Decompose
        float             planeModeAngle; // cumulative angle for SolvePlane
    };

    enum class SolveAxis { X, Y, Z };

    explicit MmdCcd(ModelPose& pose) : model(pose) {}

    // Solve a single IK chain (outer loop with distance tracking).
    inline void SolveChain(const SkeletonAsset::IKChain& chain,
                    LocalPose*    poses,
                    const SkeletonAsset&           skel,
                    uint32_t                       boneCount);

    // Single CCD iteration over all chain links.
    inline void SolveCore(uint32_t                       iteration,
                   const SkeletonAsset::IKChain&  chain,
                   LocalPose*    poses,
                   const SkeletonAsset&           skel,
                   std::vector<ChainState>&       states);

    // Single-axis solver for knee-like joints.
    inline void SolvePlane(uint32_t                       iteration,
                    size_t                         chainIdx,
                    SolveAxis                      axis,
                    const SkeletonAsset::IKChain&  chain,
                    LocalPose*    poses,
                    const SkeletonAsset&           skel,
                    std::vector<ChainState>&       states);

    DirectX::XMMATRIX ComputeWorldTransform(uint32_t bone,
        const LocalPose*, const SkeletonAsset&) const { return model.Get(bone); }

    ModelPose& model;
};
// ==== MmdCcd::SolveChain: outer loop with distance tracking (saba::Solve) ====
inline void MmdCcd::SolveChain(
    const SkeletonAsset::IKChain& chain,
    LocalPose*   poses,
    const SkeletonAsset&          skel,
    uint32_t                      boneCount)
{
    if (chain.links.empty()) return;
    for (const auto& link : chain.links) if (link.boneIndex >= boneCount) return;
    if (chain.ikBoneIndex >= boneCount || chain.targetBoneIndex >= boneCount) return;

    std::vector<ChainState> states(chain.links.size());
    for (size_t li = 0; li < chain.links.size(); ++li)
    {
        auto& s = states[li];
        uint32_t bi = chain.links[li].boneIndex;
        s.animRot       = XMLoadFloat4(&poses[bi].rot);
        s.ikRot         = XMQuaternionIdentity();
        s.saveIKRot     = XMQuaternionIdentity();
        s.prevAngle     = { 0.f, 0.f, 0.f };
        s.planeModeAngle = 0.f;

        // Reset pose to animation-only (no prior IK residue)
        XMStoreFloat4(&poses[bi].rot, s.animRot);
        model.RecomputeSubtree(bi);
    }

    // The IK bone's world position is the goal; iterate and keep the closest result.
    float maxDist = std::numeric_limits<float>::max();

    for (uint32_t iter = 0; iter < chain.loopCount; ++iter)
    {
        SolveCore(iter, chain, poses, skel, states);

        // Measure distance from effector (target bone) to IK position
        XMMATRIX targetWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
        XMMATRIX ikWorld     = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
        XMVECTOR targetPos   = targetWorld.r[3];
        XMVECTOR ikPos       = ikWorld.r[3];
        float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(targetPos, ikPos)));

        if (dist < maxDist)
        {
            maxDist = dist;
            // Closer than any earlier iteration: keep these rotations.
            for (size_t li = 0; li < chain.links.size(); ++li)
                states[li].saveIKRot = states[li].ikRot;
        }
        else
        {
            // No improvement: restore the best rotations and stop.
            for (size_t li = 0; li < chain.links.size(); ++li)
            {
                uint32_t bi = chain.links[li].boneIndex;
                states[li].ikRot = states[li].saveIKRot;
                XMVECTOR finalRot = ComposeIKAnim(states[li].ikRot, states[li].animRot);
                XMStoreFloat4(&poses[bi].rot, XMQuaternionNormalize(finalRot));
        model.RecomputeSubtree(bi);
            }
            break;
        }
    }
}

// ==== MmdCcd::SolveCore: one CCD iteration (saba::SolveCore) ====
inline void MmdCcd::SolveCore(
    uint32_t                       iteration,
    const SkeletonAsset::IKChain&  chain,
    LocalPose*    poses,
    const SkeletonAsset&           skel,
    std::vector<ChainState>&       states)
{
    // IK bone world position (the goal)
    XMMATRIX ikWorld = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
    XMVECTOR ikPos   = ikWorld.r[3];

    for (size_t li = 0; li < chain.links.size(); ++li)
    {
        const auto& link = chain.links[li];
        if (link.boneIndex >= skel.boneCount) continue;

        // Skip a link that is the target itself (zero vector; as in saba).
        if (link.boneIndex == chain.targetBoneIndex) continue;

        // A limit on exactly one axis: solve the link as a single-axis plane joint.
        if (link.hasAngleLimit)
        {
            bool xActive = (link.minAngle.x != 0.f || link.maxAngle.x != 0.f);
            bool yActive = (link.minAngle.y != 0.f || link.maxAngle.y != 0.f);
            bool zActive = (link.minAngle.z != 0.f || link.maxAngle.z != 0.f);

            if (xActive && !yActive && !zActive)
            {
                SolvePlane(iteration, li, SolveAxis::X, chain, poses, skel, states);
                continue;
            }
            if (yActive && !xActive && !zActive)
            {
                SolvePlane(iteration, li, SolveAxis::Y, chain, poses, skel, states);
                continue;
            }
            if (zActive && !xActive && !yActive)
            {
                SolvePlane(iteration, li, SolveAxis::Z, chain, poses, skel, states);
                continue;
            }
        }

        // Current effector position (target bone's world position)
        XMMATRIX effWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
        XMVECTOR effPos   = effWorld.r[3];

        XMMATRIX chainWorld = ComputeWorldTransform(link.boneIndex, poses, skel);
        XMMATRIX invChain   = XMMatrixInverse(nullptr, chainWorld);

        // Transform both positions into chain node's local space
        XMVECTOR chainIkPos  = XMVector3Transform(ikPos, invChain);
        XMVECTOR chainEffPos = XMVector3Transform(effPos, invChain);

        XMVECTOR chainIkVec  = XMVector3Normalize(chainIkPos);
        XMVECTOR chainEffVec = XMVector3Normalize(chainEffPos);

        float dot = XMVectorGetX(XMVector3Dot(chainEffVec, chainIkVec));
        dot = Clampf(dot, -1.f, 1.f);

        float angle = std::acosf(dot);
        if (angle < 1.0e-5f) continue;

        angle = Clampf(angle, -chain.angleLimit, chain.angleLimit);

        // Rotation axis (in chain node's local space)
        XMVECTOR cross = XMVector3Cross(chainEffVec, chainIkVec);
        if (XMVectorGetX(XMVector3LengthSq(cross)) < 1e-12f) continue;
        cross = XMVector3Normalize(cross);

        XMVECTOR deltaRot = XMQuaternionRotationAxis(cross, angle);

        // Combine: chainRot = ikRot * animRot * delta (saba convention)
        XMVECTOR chainRot = CombineChainRot(states[li].ikRot, states[li].animRot, deltaRot);

        if (link.hasAngleLimit)
        {
            // Decompose to Euler XYZ and clamp
            XMMATRIX chainRotMat = XMMatrixRotationQuaternion(chainRot);
            XMFLOAT4X4 rm;
            XMStoreFloat4x4(&rm, chainRotMat);

            XMFLOAT3 euler = DecomposeEuler(rm, states[li].prevAngle);

            XMFLOAT3 clamped;
            clamped.x = Clampf(euler.x, link.minAngle.x, link.maxAngle.x);
            clamped.y = Clampf(euler.y, link.minAngle.y, link.maxAngle.y);
            clamped.z = Clampf(euler.z, link.minAngle.z, link.maxAngle.z);

            // Per-iteration angle limit
            clamped.x = Clampf(clamped.x - states[li].prevAngle.x,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.x;
            clamped.y = Clampf(clamped.y - states[li].prevAngle.y,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.y;
            clamped.z = Clampf(clamped.z - states[li].prevAngle.z,
                               -chain.angleLimit, chain.angleLimit) + states[li].prevAngle.z;

            states[li].prevAngle = clamped;
            chainRot = QuatFromEulerXYZ(clamped.x, clamped.y, clamped.z);
        }

        states[li].ikRot = ExtractIKRot(chainRot, states[li].animRot);
        XMVECTOR finalRot = ComposeIKAnim(states[li].ikRot, states[li].animRot);
        XMStoreFloat4(&poses[link.boneIndex].rot, XMQuaternionNormalize(finalRot));
        model.RecomputeSubtree(link.boneIndex);
    }
}

// ==== MmdCcd::SolvePlane: single-axis solver (saba::SolvePlane) ====
inline void MmdCcd::SolvePlane(
    uint32_t                       iteration,
    size_t                         chainIdx,
    SolveAxis                      axis,
    const SkeletonAsset::IKChain&  chain,
    LocalPose*    poses,
    const SkeletonAsset&           skel,
    std::vector<ChainState>&       states)
{
    int axisIndex = 0;
    XMVECTOR rotateAxis = XMVectorSet(1, 0, 0, 0);
    switch (axis)
    {
    case SolveAxis::X: axisIndex = 0; rotateAxis = XMVectorSet(1, 0, 0, 0); break;
    case SolveAxis::Y: axisIndex = 1; rotateAxis = XMVectorSet(0, 1, 0, 0); break;
    case SolveAxis::Z: axisIndex = 2; rotateAxis = XMVectorSet(0, 0, 1, 0); break;
    }

    const auto& link = chain.links[chainIdx];
    auto& state = states[chainIdx];

    // IK bone world position (the goal)
    XMMATRIX ikWorld = ComputeWorldTransform(chain.ikBoneIndex, poses, skel);
    XMVECTOR ikPos   = ikWorld.r[3];

    // Effector position (target bone)
    XMMATRIX effWorld = ComputeWorldTransform(chain.targetBoneIndex, poses, skel);
    XMVECTOR effPos   = effWorld.r[3];

    XMMATRIX chainWorld = ComputeWorldTransform(link.boneIndex, poses, skel);
    XMMATRIX invChain   = XMMatrixInverse(nullptr, chainWorld);

    XMVECTOR chainIkPos  = XMVector3Transform(ikPos, invChain);
    XMVECTOR chainEffPos = XMVector3Transform(effPos, invChain);

    XMVECTOR chainIkVec  = XMVector3Normalize(chainIkPos);
    XMVECTOR chainEffVec = XMVector3Normalize(chainEffPos);

    float dot = XMVectorGetX(XMVector3Dot(chainEffVec, chainIkVec));
    dot = Clampf(dot, -1.f, 1.f);

    float angle = std::acosf(dot);
    angle = Clampf(angle, -chain.angleLimit, chain.angleLimit);

    // Test positive and negative rotation; pick the one closer to target
    XMVECTOR rot1 = XMQuaternionRotationAxis(rotateAxis, angle);
    XMVECTOR rot2 = XMQuaternionRotationAxis(rotateAxis, -angle);
    XMVECTOR effVec1 = XMVector3Rotate(chainEffVec, rot1);
    XMVECTOR effVec2 = XMVector3Rotate(chainEffVec, rot2);
    float dot1 = XMVectorGetX(XMVector3Dot(effVec1, chainIkVec));
    float dot2 = XMVectorGetX(XMVector3Dot(effVec2, chainIkVec));

    float newAngle = state.planeModeAngle;
    if (dot1 > dot2)
        newAngle += angle;
    else
        newAngle -= angle;

    float limitMin = (&link.minAngle.x)[axisIndex];
    float limitMax = (&link.maxAngle.x)[axisIndex];

    // First iteration: try the flipped sign to land inside the limit range (as in saba).
    if (iteration == 0)
    {
        if (newAngle < limitMin || newAngle > limitMax)
        {
            if (-newAngle >= limitMin && -newAngle <= limitMax)
            {
                newAngle = -newAngle;
            }
            else
            {
                float half = (limitMin + limitMax) * 0.5f;
                if (std::fabsf(half - newAngle) > std::fabsf(half + newAngle))
                    newAngle = -newAngle;
            }
        }
    }

    newAngle = Clampf(newAngle, limitMin, limitMax);
    state.planeModeAngle = newAngle;

    // IK rotation = RotateAxis(newAngle) * inv(animRot) (saba convention)
    // DXMath: ikRot = Multiply(conj(animRot), RotAxis(newAngle))
    XMVECTOR axisQuat = XMQuaternionRotationAxis(rotateAxis, newAngle);
    state.ikRot = ExtractIKRot(axisQuat, state.animRot);

    XMVECTOR finalRot = ComposeIKAnim(state.ikRot, state.animRot);
    XMStoreFloat4(&poses[link.boneIndex].rot, XMQuaternionNormalize(finalRot));
        model.RecomputeSubtree(link.boneIndex);
}
}
