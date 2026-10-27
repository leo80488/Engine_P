#pragma once
#include "Anim/IK/TwoBone.h"
namespace Anim::IK {
inline bool ValidChain(const ModelPose& pose,const std::vector<uint32_t>& bones) {
    using namespace DirectX;
    if(bones.size()<2)return false;
    for(size_t i=0;i<bones.size();++i) {
        if(bones[i]>=pose.skel.boneCount)return false;
        if(i && pose.skel.parentIndex[bones[i]]!=int32_t(bones[i-1]))return false;
        if(i && DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(pose.Get(bones[i]).r[3]-pose.Get(bones[i-1]).r[3]))<1e-12f)return false;
    }
    return true;
}
inline bool SolveCCD(ModelPose& pose,const std::vector<uint32_t>& bones,DirectX::FXMVECTOR target,unsigned iterations=32) {
    using namespace DirectX;
    if(!Finite3(target) || !ValidChain(pose,bones))return false;
    for(unsigned n=0;n<iterations;++n) {
        for(size_t i=bones.size()-1;i-->0;) {
            auto root=pose.Get(bones[i]).r[3];
            pose.ApplyModelRotationDelta(bones[i],FromTo(pose.Get(bones.back()).r[3]-root,target-root));
        }
        if(XMVectorGetX(XMVector3LengthSq(pose.Get(bones.back()).r[3]-target))<1e-8f)break;
    }
    return true;
}
inline bool SolveFABRIK(ModelPose& pose,const std::vector<uint32_t>& bones,DirectX::FXMVECTOR target,unsigned iterations=32) {
    using namespace DirectX;
    if(!Finite3(target) || !ValidChain(pose,bones))return false;
    std::vector<XMVECTOR> points;std::vector<float> lengths;float total=0;
    for(auto b:bones)points.push_back(pose.Get(b).r[3]);
    for(size_t i=1;i<points.size();++i) {auto l=XMVectorGetX(XMVector3Length(points[i]-points[i-1]));lengths.push_back(l);total+=l;}
    const auto root=points[0];
    auto direction=[](XMVECTOR a,XMVECTOR fallback) {
        return XMVectorGetX(XMVector3LengthSq(a))>1e-12f?XMVector3Normalize(a):fallback;
    };
    if(XMVectorGetX(XMVector3Length(target-root))>=total) {
        auto axis=direction(target-root,XMVectorSet(0,1,0,0));
        for(size_t i=1;i<points.size();++i)points[i]=points[i-1]+axis*lengths[i-1];
    } else for(unsigned n=0;n<iterations;++n) {
        points.back()=target;
        for(size_t i=points.size()-1;i-->0;)points[i]=points[i+1]+direction(points[i]-points[i+1],XMVectorSet(0,1,0,0))*lengths[i];
        points[0]=root;
        for(size_t i=1;i<points.size();++i)points[i]=points[i-1]+direction(points[i]-points[i-1],XMVectorSet(0,-1,0,0))*lengths[i-1];
        if(XMVectorGetX(XMVector3LengthSq(points.back()-target))<1e-8f)break;
    }
    XMVECTOR s,endRotation,t;XMMatrixDecompose(&s,&endRotation,&t,pose.Get(bones.back()));
    for(size_t i=0;i+1<bones.size();++i)
        pose.ApplyModelRotationDelta(bones[i],FromTo(pose.Get(bones[i+1]).r[3]-pose.Get(bones[i]).r[3],points[i+1]-points[i]));
    XMVECTOR current;XMMatrixDecompose(&s,&current,&t,pose.Get(bones.back()));
    pose.ApplyModelRotationDelta(bones.back(),XMQuaternionMultiply(XMQuaternionInverse(current),endRotation));
    return true;
}
inline bool SolveLookAt(ModelPose& pose,uint32_t bone,DirectX::FXMVECTOR target,
                        DirectX::FXMVECTOR axis,float maxDegrees) {
    using namespace DirectX;
    if(bone>=pose.skel.boneCount || !Finite3(target) || !Finite3(axis) || XMVectorGetX(XMVector3LengthSq(axis))<1e-12f)return false;
    auto direction=target-pose.Get(bone).r[3];
    if(XMVectorGetX(XMVector3LengthSq(direction))<1e-12f)return true;
    auto delta=FromTo(XMVector3TransformNormal(axis,pose.Get(bone)),direction);
    float angle=2.f*std::acos(std::clamp(XMVectorGetW(delta),0.f,1.f));
    float limit=XMConvertToRadians(std::clamp(maxDegrees,0.f,180.f));
    if(angle>limit && angle>1e-6f)delta=XMQuaternionSlerp(XMQuaternionIdentity(),delta,limit/angle);
    pose.ApplyModelRotationDelta(bone,delta);return true;
}
}
