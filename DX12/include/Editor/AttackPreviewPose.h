#pragma once
#include "Editor/PreviewSkin.h"
namespace AttackEditor {
inline void SamplePreviewPose(const SkeletonAsset& skeleton,const ClipAsset& clip,float seconds,
    std::vector<DirectX::XMFLOAT4X4>& global,std::vector<DirectX::XMFLOAT4X4>& skin,
    std::vector<DirectX::XMFLOAT4>& rotations) {
    std::vector<Anim::LocalPose> sampled(skeleton.boneCount),scratch;
    Anim::PoseOps::RefPose(skeleton,sampled.data());
    Anim::PoseOps::SampleClip(clip,seconds,skeleton.boneCount,sampled.data(),false);
    Editor::BuildPreviewSkin(skeleton,sampled,scratch,global,skin);
    rotations.resize(skeleton.boneCount);
    for(uint32_t b=0;b<skeleton.boneCount;++b)rotations[b]=scratch[b].rot;
}
}
