#pragma once

// RetargetMath - the one piece of math retargeting must get right. Pure and
// header-only.
//
// Engine quaternion convention (CRITICAL):
//   worldRot(bone) = XMQuaternionMultiply(localRot, parentWorldRot)
// i.e. Mul(a,b) = XMQuaternionMultiply(a,b) means "rotation a THEN rotation b",
// matching the engine's row-major `local * parent` compose (AnimationSystem).
// The three.js/USD retarget formula is column-vector; it is reversed here.
//
// RetargetLocalRotation is the SIMPLIFIED per-bone ("auxiliary pose")
// primitive: algebraically correct only when the clip's local rotation is
// relative to the RECONSTRUCTED rest. Real clips are relative to the skeleton's
// ACTUAL rest, so the baker (RetargetBaker.cpp) uses a full-pose variant that
// keeps actual rest (Bw) and reconstructed rest (Rw) distinct. Keep this
// primitive as the reference; do not bake real clips with it.
// ReconstructOrientedRestRef IS used by the baker.
//
// Bind-relative delta (rotation only): from a source bone's animated LOCAL
// rotation and the reconstructed oriented REST world rotations of the source
// bone, source parent, target bone and target parent, produce the target bone's
// LOCAL rotation. Boundary conditions:
//   * source skeleton == target skeleton  =>  trgLocal == srcLocal (identity)
//   * srcLocal == source rest local       =>  trgLocal == target rest local
// The A-pose <-> T-pose difference lives entirely in the rest rotations, so a
// correct delta absorbs it; a naive local copy does not.

#include <DirectXMath.h>
#include <cmath>

namespace Resource
{
    // srcRest       = source bone   rest world rotation (reconstructed oriented bind)
    // srcParentRest = source parent rest world rotation
    // trgRest       = target bone   rest world rotation
    // trgParentRest = target parent rest world rotation
    // srcLocal      = source bone animated LOCAL rotation (from the clip)
    inline DirectX::XMVECTOR RetargetLocalRotation(
        DirectX::FXMVECTOR srcRest,  DirectX::FXMVECTOR srcParentRest,
        DirectX::FXMVECTOR trgRest,  DirectX::GXMVECTOR trgParentRest,
        DirectX::HXMVECTOR srcLocal)
    {
        using namespace DirectX;
        // trgLocal = trgRest * invS * srcLocal * srcParentRest * invTP   (engine "a THEN b" order)
        const XMVECTOR invS  = XMQuaternionInverse(srcRest);
        const XMVECTOR invTP = XMQuaternionInverse(trgParentRest);
        XMVECTOR q = XMQuaternionMultiply(trgRest, invS);
        q = XMQuaternionMultiply(q, srcLocal);
        q = XMQuaternionMultiply(q, srcParentRest);
        q = XMQuaternionMultiply(q, invTP);
        return XMQuaternionNormalize(q);
    }

    // Reconstruct a deterministic oriented rest rotation for a bone that points
    // along its local +Y toward @p childPos, with a Gram-Schmidt roll axis. This
    // MUST match HumanoidProfileBuilder's reconstruction so source / target frames
    // are consistent (the delta only cancels when both use the same convention).
    // Returns identity when the bone is degenerate (no length to the child).
    // @p refUp is the roll reference: world-up (0,1,0) for roughly horizontal bones
    // (arms), world-forward (0,0,1) for near-VERTICAL bones (spine / neck / head /
    // legs). World-up is degenerate there: two rigs whose spines lean by a few
    // degrees cross its |dot| > 0.99 fallback inconsistently (one hits it, the
    // other does not), flipping the roll and tilting the torso.
    inline DirectX::XMVECTOR ReconstructOrientedRestRef(DirectX::FXMVECTOR bonePos,
                                                        DirectX::FXMVECTOR childPos,
                                                        DirectX::FXMVECTOR refUp)
    {
        using namespace DirectX;
        XMVECTOR dir = XMVectorSubtract(childPos, bonePos);
        if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-12f)
            return XMQuaternionIdentity();

        XMVECTOR primary = XMVector3Normalize(dir);                 // local +Y
        XMVECTOR up      = refUp;
        if (std::fabs(XMVectorGetX(XMVector3Dot(primary, up))) > 0.99f)
            up = (std::fabs(XMVectorGetY(primary)) > 0.9f)          // still near-parallel: pick a stable perp
                 ? XMVectorSet(0.f, 0.f, 1.f, 0.f)
                 : XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMVECTOR side = XMVector3Normalize(XMVector3Cross(up, primary)); // local +X
        XMVECTOR up2  = XMVector3Cross(side, primary);                    // local +Z (right-handed: side x primary)

        XMMATRIX basis(
            XMVectorSetW(side,    0.f),
            XMVectorSetW(primary, 0.f),
            XMVectorSetW(up2,     0.f),
            XMVectorSet(0.f, 0.f, 0.f, 1.f));
        return XMQuaternionNormalize(XMQuaternionRotationMatrix(basis));
    }

    // Overload with the world-up reference (fine for arms / hands / feet).
    inline DirectX::XMVECTOR ReconstructOrientedRest(DirectX::FXMVECTOR bonePos,
                                                     DirectX::FXMVECTOR childPos)
    {
        return ReconstructOrientedRestRef(bonePos, childPos, DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f));
    }
}
