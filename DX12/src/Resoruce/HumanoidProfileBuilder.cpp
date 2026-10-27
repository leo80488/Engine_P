#include "Resource/HumanoidProfileBuilder.h"
#include "Resource/HumanoidNameTable.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace Resource
{
    namespace
    {
        // Canonical "down the bone" child role, used to orient each bone toward
        // the continuation of its limb. Count = leaf (no directional child).
        HumanoidBone DirChildRole(HumanoidBone r)
        {
            switch (r)
            {
            case HumanoidBone::Hips:        return HumanoidBone::Spine;
            case HumanoidBone::Spine:       return HumanoidBone::Chest;
            case HumanoidBone::Chest:       return HumanoidBone::UpperChest;
            case HumanoidBone::UpperChest:  return HumanoidBone::Neck;
            case HumanoidBone::Neck:        return HumanoidBone::Head;
            case HumanoidBone::LeftShoulder:  return HumanoidBone::LeftUpperArm;
            case HumanoidBone::LeftUpperArm:  return HumanoidBone::LeftLowerArm;
            case HumanoidBone::LeftLowerArm:  return HumanoidBone::LeftHand;
            case HumanoidBone::RightShoulder: return HumanoidBone::RightUpperArm;
            case HumanoidBone::RightUpperArm: return HumanoidBone::RightLowerArm;
            case HumanoidBone::RightLowerArm: return HumanoidBone::RightHand;
            case HumanoidBone::LeftUpperLeg:  return HumanoidBone::LeftLowerLeg;
            case HumanoidBone::LeftLowerLeg:  return HumanoidBone::LeftFoot;
            case HumanoidBone::LeftFoot:      return HumanoidBone::LeftToes;
            case HumanoidBone::RightUpperLeg: return HumanoidBone::RightLowerLeg;
            case HumanoidBone::RightLowerLeg: return HumanoidBone::RightFoot;
            case HumanoidBone::RightFoot:     return HumanoidBone::RightToes;
            // Fingers
            case HumanoidBone::LeftThumbMetacarpal: return HumanoidBone::LeftThumbProximal;
            case HumanoidBone::LeftThumbProximal:   return HumanoidBone::LeftThumbDistal;
            case HumanoidBone::LeftIndexProximal:   return HumanoidBone::LeftIndexIntermediate;
            case HumanoidBone::LeftIndexIntermediate:return HumanoidBone::LeftIndexDistal;
            case HumanoidBone::LeftMiddleProximal:   return HumanoidBone::LeftMiddleIntermediate;
            case HumanoidBone::LeftMiddleIntermediate:return HumanoidBone::LeftMiddleDistal;
            case HumanoidBone::LeftRingProximal:     return HumanoidBone::LeftRingIntermediate;
            case HumanoidBone::LeftRingIntermediate: return HumanoidBone::LeftRingDistal;
            case HumanoidBone::LeftLittleProximal:   return HumanoidBone::LeftLittleIntermediate;
            case HumanoidBone::LeftLittleIntermediate:return HumanoidBone::LeftLittleDistal;
            case HumanoidBone::RightThumbMetacarpal: return HumanoidBone::RightThumbProximal;
            case HumanoidBone::RightThumbProximal:   return HumanoidBone::RightThumbDistal;
            case HumanoidBone::RightIndexProximal:   return HumanoidBone::RightIndexIntermediate;
            case HumanoidBone::RightIndexIntermediate:return HumanoidBone::RightIndexDistal;
            case HumanoidBone::RightMiddleProximal:   return HumanoidBone::RightMiddleIntermediate;
            case HumanoidBone::RightMiddleIntermediate:return HumanoidBone::RightMiddleDistal;
            case HumanoidBone::RightRingProximal:     return HumanoidBone::RightRingIntermediate;
            case HumanoidBone::RightRingIntermediate: return HumanoidBone::RightRingDistal;
            case HumanoidBone::RightLittleProximal:   return HumanoidBone::RightLittleIntermediate;
            case HumanoidBone::RightLittleIntermediate:return HumanoidBone::RightLittleDistal;
            default: return HumanoidBone::Count; // leaf
            }
        }

        // Model-space rest position of a bone = translation of its bindPose
        // (bone -> model). PMX bindPose is a pure translation; FBX / glTF may carry
        // rotation too, but the translation column is the position either way.
        XMVECTOR BonePos(const SkeletonAsset& sk, int32_t b)
        {
            const XMFLOAT4X4& m = sk.bindPose[b];
            return XMVectorSet(m._41, m._42, m._43, 0.f);
        }

        // Reconstruct a deterministic oriented rest rotation for a bone that
        // points along +Y toward `childPos`, with a Gram-Schmidt roll axis.
        XMVECTOR ReconstructRestRot(XMVECTOR bonePos, XMVECTOR childPos)
        {
            XMVECTOR dir = XMVectorSubtract(childPos, bonePos);
            if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-12f)
                return XMQuaternionIdentity();

            XMVECTOR primary = XMVector3Normalize(dir);                 // local +Y
            XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            if (std::fabs(XMVectorGetX(XMVector3Dot(primary, up))) > 0.99f)
                up = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            XMVECTOR side    = XMVector3Normalize(XMVector3Cross(up, primary)); // local +X
            XMVECTOR up2     = XMVector3Cross(side, primary);                    // local +Z (right-handed)

            // Rows = world directions of the local axes (row-vector convention,
            // matching the engine's parent*local row-major compose).
            XMMATRIX basis(
                XMVectorSetW(side,    0.f),
                XMVectorSetW(primary, 0.f),
                XMVectorSetW(up2,     0.f),
                XMVectorSet(0.f, 0.f, 0.f, 1.f));
            return XMQuaternionNormalize(XMQuaternionRotationMatrix(basis));
        }
    } // namespace

    void HumanoidProfileBuilder::Build(SkeletonAsset& skel, HumanoidConvention conv)
    {
        skel.humanoid.Reset();

        if (conv == HumanoidConvention::Unknown)
            conv = DetectConvention(skel);
        skel.humanoid.convention = static_cast<uint8_t>(conv);

        if (conv == HumanoidConvention::Unknown)
        {
            // No recognizable humanoid naming: leave the profile empty / invalid.
            FinalizeDerived(skel);
            return;
        }

        // Map every bone to a role; the first bone claiming a role wins (exact-table
        // conventions never collide; the generic path may, and first-wins keeps it
        // deterministic). Legs map to the BASE FK bones; the D / EX deform chain is driven
        // by the runtime grant, never mapped (HumanoidNameTable kPmx). Real bones only: a
        // virtual bone has no role.
        for (uint32_t i = 0; i < skel.RealBoneCount(); ++i)
        {
            HumanoidBone role = MapBoneNameToRole(conv, skel.boneNames[i]);
            if (role == HumanoidBone::Count) continue;
            if (skel.humanoid.boneForRole[static_cast<uint32_t>(role)] < 0)
                skel.humanoid.boneForRole[static_cast<uint32_t>(role)] = static_cast<int32_t>(i);
        }

        // Hips translation split (PMX): root motion lives on the center bone, not on the
        // Hips rotation bone (lower body). Other conventions keep -1 = same as Hips.
        if (conv == HumanoidConvention::PmxJp)
        {
            for (uint32_t i = 0; i < skel.RealBoneCount(); ++i)
                if (std::strcmp(skel.boneNames[i], "センター") == 0)
                { skel.humanoid.hipsTranslationBone = static_cast<int32_t>(i); break; }
        }

        FinalizeDerived(skel);
    }

    void HumanoidProfileBuilder::FinalizeDerived(SkeletonAsset& skel)
    {
        HumanoidProfile& hp = skel.humanoid;

        // Oriented rest rotation per mapped role bone.
        for (uint32_t ri = 0; ri < kHumanoidBoneCount; ++ri)
        {
            const int32_t b = hp.boneForRole[ri];
            if (b < 0 || static_cast<uint32_t>(b) >= skel.boneCount)
            {
                hp.restModelRot[ri] = XMFLOAT4(0.f, 0.f, 0.f, 1.f);
                continue;
            }

            const HumanoidBone role = static_cast<HumanoidBone>(ri);
            const XMVECTOR bonePos = BonePos(skel, b);

            // Prefer the canonical limb-continuation child (by role, position-based
            // so intermediate unmapped bones don't matter); fall back to the first
            // hierarchy child; else treat as a leaf.
            int32_t childBone = -1;
            HumanoidBone rc = DirChildRole(role);
            if (rc != HumanoidBone::Count && hp.Has(rc))
                childBone = hp.Bone(rc);
            if (childBone < 0)
            {
                for (uint32_t j = 0; j < skel.RealBoneCount(); ++j)   // a virtual child orients nothing
                    if (skel.parentIndex[j] == b) { childBone = static_cast<int32_t>(j); break; }
            }

            if (childBone < 0)
                hp.restModelRot[ri] = XMFLOAT4(0.f, 0.f, 0.f, 1.f); // leaf -> identity
            else
                XMStoreFloat4(&hp.restModelRot[ri],
                              ReconstructRestRot(bonePos, BonePos(skel, childBone)));
        }

        // Hip-JOINT height above the floor = model-space Y of the upper-leg (thigh) roots,
        // averaged L/R: the stable metric for translation (stride) scaling. The Hips role
        // bone's pivot varies by rig (on MMD it is the lower-body bone, not a hip joint).
        // Falls back to Hips, then 0.
        auto boneY = [&](HumanoidBone r) { return XMVectorGetY(BonePos(skel, hp.Bone(r))); };
        hp.hipHeight = 0.f;
        if (hp.Has(HumanoidBone::LeftUpperLeg) && hp.Has(HumanoidBone::RightUpperLeg))
            hp.hipHeight = 0.5f * (boneY(HumanoidBone::LeftUpperLeg) + boneY(HumanoidBone::RightUpperLeg));
        else if (hp.Has(HumanoidBone::LeftUpperLeg))
            hp.hipHeight = boneY(HumanoidBone::LeftUpperLeg);
        else if (hp.Has(HumanoidBone::Hips))
            hp.hipHeight = boneY(HumanoidBone::Hips);

        // Valid iff all 15 required roles are mapped.
        bool ok = true;
        for (uint32_t ri = 0; ri < kHumanoidBoneCount && ok; ++ri)
            if (IsRequiredHumanoidBone(static_cast<HumanoidBone>(ri)) && hp.boneForRole[ri] < 0)
                ok = false;
        hp.valid = ok ? 1 : 0;
    }
}
