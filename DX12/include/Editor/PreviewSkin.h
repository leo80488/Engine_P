#pragma once
#include "Resource/SkeletonAsset.h"
#include "Anim/IK/Kinematics.h"
#include <span>

namespace Editor {
// Builds global and skin matrices from source (a graph task result before grants / IK):
// copies it into scratch, applies grants and pins root translation to the rest pose.
// Never mutates the live instance; throws when the bone count does not match.
inline void BuildPreviewSkin(const SkeletonAsset& skeleton, std::span<const Anim::LocalPose> source,
    std::vector<Anim::LocalPose>& scratch, std::vector<DirectX::XMFLOAT4X4>& global,
    std::vector<DirectX::XMFLOAT4X4>& skin) {
    using namespace DirectX;
    if (source.size() != skeleton.boneCount) throw std::runtime_error("Preview skeleton does not match graph pose");
    scratch.assign(source.begin(),source.end());
    Anim::PoseOps::ApplyGrants(skeleton,scratch.data(),skeleton.boneCount,false);
    Anim::PoseOps::ApplyGrants(skeleton,scratch.data(),skeleton.boneCount,true);
    global.resize(skeleton.boneCount);skin.resize(skeleton.boneCount);
    for (uint32_t b=0;b<skeleton.boneCount;++b) {
        auto& pose=scratch[b];const int parent=skeleton.parentIndex[b];
        if(parent<0){XMVECTOR scale,rotation,position;XMMatrixDecompose(&scale,&rotation,&position,XMLoadFloat4x4(&skeleton.restPoseLocal[b]));XMStoreFloat3(&pose.pos,position);}
        const auto local=Anim::IK::LocalMatrix(pose);
        const auto world=parent>=0?local*XMLoadFloat4x4(&global[parent]):local;
        XMStoreFloat4x4(&global[b],world);XMStoreFloat4x4(&skin[b],XMLoadFloat4x4(&skeleton.inverseBindPose[b])*world);
    }
}
}
