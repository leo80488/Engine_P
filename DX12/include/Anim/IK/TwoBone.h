#pragma once
#include "Anim/IK/Kinematics.h"
#include <cmath>

namespace Anim::IK {
inline bool Finite3(DirectX::FXMVECTOR v)
{
    DirectX::XMFLOAT3 p; DirectX::XMStoreFloat3(&p,v);
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
inline DirectX::XMVECTOR FromTo(DirectX::FXMVECTOR from, DirectX::FXMVECTOR to)
{
    using namespace DirectX;
    if (XMVectorGetX(XMVector3LengthSq(from)) < 1e-12f ||
        XMVectorGetX(XMVector3LengthSq(to)) < 1e-12f) return XMQuaternionIdentity();
    auto a=XMVector3Normalize(from), b=XMVector3Normalize(to);
    const float dot=std::clamp(XMVectorGetX(XMVector3Dot(a,b)),-1.f,1.f);
    // Keep small swings: a 0.001-radian dead zone becomes centimetres on
    // large PMX rigs. The cross product still resolves them when dot rounds 1.
    const auto cross=XMVector3Cross(a,b);
    if (dot>0.f && XMVectorGetX(XMVector3LengthSq(cross))<1e-20f) return XMQuaternionIdentity();
    if (dot < -0.999999f) {
        auto axis=XMVector3Cross(a, std::abs(XMVectorGetX(a)) < 0.8f ? XMVectorSet(1,0,0,0) : XMVectorSet(0,1,0,0));
        return XMQuaternionRotationAxis(XMVector3Normalize(axis),XM_PI);
    }
    return XMQuaternionNormalize(XMVectorSetW(cross,1.f+dot));
}
inline float SoftDistance(float distance, float length, float softness)
{
    const float zone=length*std::clamp(softness,0.f,0.5f);
    if (zone<=1e-6f || distance<=length-zone) return std::min(distance,length);
    return length-zone*std::exp(-(distance-(length-zone))/zone);
}
inline bool SolveTwoBone(ModelPose& pose, uint32_t root, uint32_t mid, uint32_t end,
                         DirectX::FXMVECTOR target, DirectX::FXMVECTOR pole, float softness)
{
    using namespace DirectX;
    if (root>=pose.skel.boneCount || mid>=pose.skel.boneCount || end>=pose.skel.boneCount
        || !Finite3(target) || !Finite3(pole)) return false;
    const auto a=pose.Get(root).r[3], b=pose.Get(mid).r[3], c=pose.Get(end).r[3];
    const float l1=XMVectorGetX(XMVector3Length(b-a)), l2=XMVectorGetX(XMVector3Length(c-b));
    if (!std::isfinite(l1+l2) || l1<1e-6f || l2<1e-6f) return false;
    const float raw=XMVectorGetX(XMVector3Length(target-a));
    auto axis=raw>1e-6f ? XMVector3Normalize(target-a) : XMVector3Normalize(c-a);
    if (!Finite3(axis)) axis=XMVectorSet(0,1,0,0);
    const float distance=std::clamp(SoftDistance(raw,l1+l2,softness),std::abs(l1-l2)+1e-6f,l1+l2-1e-6f);
    auto bend=pole-a-axis*XMVector3Dot(pole-a,axis);
    if (XMVectorGetX(XMVector3LengthSq(bend)) < 1e-10f)
        bend=XMVector3Cross(axis,std::abs(XMVectorGetX(axis))<0.8f ? XMVectorSet(1,0,0,0) : XMVectorSet(0,0,1,0));
    bend=XMVector3Normalize(bend);
    const float x=(l1*l1-l2*l2+distance*distance)/(2.f*distance);
    const float y=std::sqrt(std::max(0.f,l1*l1-x*x));
    auto joint=a+axis*x+bend*y, effector=a+axis*distance;
    XMVECTOR s,endRotation,t;
    XMMatrixDecompose(&s,&endRotation,&t,pose.Get(end));
    pose.ApplyModelRotationDelta(root,FromTo(b-a,joint-a));
    pose.ApplyModelRotationDelta(mid,FromTo(pose.Get(end).r[3]-pose.Get(mid).r[3],effector-pose.Get(mid).r[3]));
    XMVECTOR current;
    XMMatrixDecompose(&s,&current,&t,pose.Get(end));
    pose.ApplyModelRotationDelta(end,XMQuaternionMultiply(XMQuaternionInverse(current),endRotation));
    return true;
}
inline void GlobalMatrices(const LocalPose* poses,const std::vector<int>& parents,std::vector<DirectX::XMFLOAT4X4>& matrices) {
    using namespace DirectX;matrices.resize(parents.size());for(size_t b=0;b<parents.size();++b){auto m=LocalMatrix(poses[b]);int p=parents[b];if(p>=0 && size_t(p)<b)m*=XMLoadFloat4x4(&matrices[p]);XMStoreFloat4x4(&matrices[b],m);}
}
inline bool SolveTwoBone(LocalPose* poses,const std::vector<int>& parents,int root,int mid,int end,const DirectX::XMFLOAT3& target,const DirectX::XMFLOAT3& pole,float weight,std::vector<DirectX::XMFLOAT4X4>& fk,std::vector<DirectX::XMFLOAT4>& rotations) {
    using namespace DirectX;if(root<0 || mid<0 || end<0 || size_t(root)>=parents.size() || size_t(mid)>=parents.size() || size_t(end)>=parents.size() || parents[mid]!=root || parents[end]!=mid || weight<=0)return false;
    auto rootBefore=poses[root].rot,midBefore=poses[mid].rot;GlobalMatrices(poses,parents,fk);
    auto position=[&](int b){return XMVectorSet(fk[b]._41,fk[b]._42,fk[b]._43,0);};auto a=position(root),b=position(mid),c=position(end),goal=XMLoadFloat3(&target);
    float l1=XMVectorGetX(XMVector3Length(b-a)),l2=XMVectorGetX(XMVector3Length(c-b)),distance=XMVectorGetX(XMVector3Length(goal-a));if(l1<1e-6f || l2<1e-6f || distance<1e-6f)return false;
    auto direction=XMVector3Normalize(goal-a);float d=std::clamp(distance,std::fabs(l1-l2)+1e-5f,l1+l2-1e-5f),x=(l1*l1-l2*l2+d*d)/(2*d),height=std::sqrt(std::max(0.f,l1*l1-x*x));
    auto normal=XMLoadFloat3(&pole)-a;normal-=direction*XMVector3Dot(normal,direction);
    if(XMVectorGetX(XMVector3LengthSq(normal))<1e-6f){normal=b-a;normal-=direction*XMVector3Dot(normal,direction);}
    if(XMVectorGetX(XMVector3LengthSq(normal))<1e-6f){normal=XMVector3Cross(direction,XMVectorSet(0,1,0,0));if(XMVectorGetX(XMVector3LengthSq(normal))<1e-6f)normal=XMVector3Cross(direction,XMVectorSet(1,0,0,0));}
    auto desiredMid=a+direction*x+XMVector3Normalize(normal)*height,desiredEnd=a+direction*d;
    auto rotate=[&](int bone,XMVECTOR from,XMVECTOR to){GlobalRotations(poses,parents,rotations);auto q=XMQuaternionMultiply(XMLoadFloat4(&rotations[bone]),FromTo(from,to));int parent=parents[bone];if(parent>=0)q=XMQuaternionMultiply(q,XMQuaternionInverse(XMLoadFloat4(&rotations[parent])));XMStoreFloat4(&poses[bone].rot,XMQuaternionNormalize(q));};
    rotate(root,b-a,desiredMid-a);GlobalMatrices(poses,parents,fk);b=position(mid);c=position(end);rotate(mid,c-b,desiredEnd-b);
    poses[root].rot=PoseOps::Slerp4(rootBefore,poses[root].rot,std::clamp(weight,0.f,1.f));poses[mid].rot=PoseOps::Slerp4(midBefore,poses[mid].rot,std::clamp(weight,0.f,1.f));return true;
}
}
