#pragma once
#include "Anim/IK/LocalPose.h"
#include "Anim/Core/PoseOps.h"
#include "Resource/SkeletonAsset.h"
#include <algorithm>
#include <vector>

namespace Anim::IK
{
    inline DirectX::XMMATRIX LocalMatrix(const LocalPose& pose)
    {
        using namespace DirectX;
        return XMMatrixScaling(pose.scl.x, pose.scl.y, pose.scl.z)
             * XMMatrixRotationQuaternion(XMLoadFloat4(&pose.rot))
             * XMMatrixTranslation(pose.pos.x, pose.pos.y, pose.pos.z);
    }

    // Root translation belongs to the entity, matching the skinning contract.
    inline DirectX::XMMATRIX LocalMatrix(uint32_t bone, const LocalPose* poses,
                                        const SkeletonAsset& skeleton)
    {
        if (skeleton.parentIndex[bone] >= 0) return LocalMatrix(poses[bone]);
        DirectX::XMVECTOR scale, rotation, translation;
        DirectX::XMMatrixDecompose(&scale, &rotation, &translation,
            DirectX::XMLoadFloat4x4(&skeleton.restPoseLocal[bone]));
        auto root = poses[bone];
        DirectX::XMStoreFloat3(&root.pos, translation);
        return LocalMatrix(root);
    }

    inline DirectX::XMMATRIX BoneModel(uint32_t bone, const LocalPose* poses,
                                     const SkeletonAsset& skeleton)
    {
        using namespace DirectX;
        if (!poses || bone >= skeleton.boneCount) return XMMatrixIdentity();
        std::vector<uint32_t> ancestors;
        for (;;)
        {
            ancestors.push_back(bone);
            const int32_t parent = skeleton.parentIndex[bone];
            if (parent < 0 || static_cast<uint32_t>(parent) >= bone) break;
            bone = static_cast<uint32_t>(parent);
        }
        XMMATRIX result = XMMatrixIdentity();
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            result = LocalMatrix(*it, poses, skeleton) * result;
        return result;
    }

    class ModelPose
    {
    public:
        ModelPose(LocalPose* poses, const SkeletonAsset& skeleton)
            : local(poses), skel(skeleton), matrices(skeleton.boneCount) { Rebuild(); }
        void Rebuild() { RecomputeSubtree(0, true); }
        void RecomputeSubtree(uint32_t root, bool all = false)
        {
            using namespace DirectX;
            affected.resize(skel.boneCount);
            std::fill(affected.begin(), affected.end(), false);
            for (uint32_t b = root; b < skel.boneCount; ++b)
            {
                const auto parent = skel.parentIndex[b];
                if (!all && b != root && !(parent >= 0 && affected[parent])) continue;
                affected[b] = true;
                auto matrix = LocalMatrix(b, local, skel);
                if (parent >= 0 && static_cast<uint32_t>(parent) < b)
                    matrix = matrix * XMLoadFloat4x4(&matrices[parent]);
                XMStoreFloat4x4(&matrices[b], matrix);
            }
        }
        DirectX::XMMATRIX Get(uint32_t b) const { return DirectX::XMLoadFloat4x4(&matrices[b]); }
        void ApplyModelRotationDelta(uint32_t bone, DirectX::FXMVECTOR delta)
        {
            using namespace DirectX;
            XMVECTOR s, r, t;
            XMMatrixDecompose(&s, &r, &t, Get(bone));
            auto rotated = XMMatrixRotationQuaternion(XMQuaternionMultiply(r, delta));
            const auto parent = skel.parentIndex[bone];
            if (parent >= 0)
            {
                XMVECTOR ps, pr, pt;
                XMMatrixDecompose(&ps, &pr, &pt, Get(parent));
                rotated = rotated * XMMatrixTranspose(XMMatrixRotationQuaternion(pr));
            }
            XMStoreFloat4(&local[bone].rot, XMQuaternionNormalize(XMQuaternionRotationMatrix(rotated)));
            RecomputeSubtree(bone);
        }
        LocalPose* local;
        const SkeletonAsset& skel;
        std::vector<DirectX::XMFLOAT4X4> matrices;
    private:
        std::vector<bool> affected;
    };
inline void GlobalRotations(const LocalPose* poses,const std::vector<int>& parents,std::vector<DirectX::XMFLOAT4>& out) {
    using namespace DirectX;out.resize(parents.size());
    for(size_t b=0;b<parents.size();++b){auto q=XMLoadFloat4(&poses[b].rot);int p=parents[b];if(p>=0 && size_t(p)<b)q=XMQuaternionMultiply(q,XMLoadFloat4(&out[p]));XMStoreFloat4(&out[b],XMQuaternionNormalize(q));}
}
inline void MeshAdditive(LocalPose* base,const LocalPose* target,const LocalPose* reference,const std::vector<int>& parents,float alpha,const float* mask,std::vector<DirectX::XMFLOAT4>& a,std::vector<DirectX::XMFLOAT4>& b,std::vector<DirectX::XMFLOAT4>& r) {
    using namespace DirectX;GlobalRotations(base,parents,a);GlobalRotations(target,parents,b);GlobalRotations(reference,parents,r);
    PoseOps::Additive(base,target,reference,uint32_t(parents.size()),alpha,mask);
    for(size_t bone=0;bone<parents.size();++bone){auto delta=XMQuaternionMultiply(XMQuaternionInverse(XMLoadFloat4(&r[bone])),XMLoadFloat4(&b[bone]));auto q=XMQuaternionMultiply(XMLoadFloat4(&a[bone]),XMQuaternionSlerp(XMQuaternionIdentity(),delta,alpha*(mask?mask[bone]:1.f)));XMStoreFloat4(&a[bone],XMQuaternionNormalize(q));}
    for(size_t bone=0;bone<parents.size();++bone){auto q=XMLoadFloat4(&a[bone]);int p=parents[bone];if(p>=0 && size_t(p)<bone)q=XMQuaternionMultiply(q,XMQuaternionInverse(XMLoadFloat4(&a[p])));XMStoreFloat4(&base[bone].rot,XMQuaternionNormalize(q));}
}
}
