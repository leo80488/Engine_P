#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
namespace Anim {
struct RootMotionKey {float time=0;DirectX::XMFLOAT3 position{};float yaw=0;};
struct RootMotionDelta {DirectX::XMFLOAT3 translation{};float yaw=0;};
struct FloatCurveKey {float time=0,value=0;};
struct FloatCurve {std::string name;std::vector<FloatCurveKey> keys;};
inline float SampleCurve(const FloatCurve& curve,float time) {
    if(curve.keys.empty())return 0;if(time<=curve.keys.front().time)return curve.keys.front().value;
    auto next=std::upper_bound(curve.keys.begin(),curve.keys.end(),time,[](float t,const auto& k){return t<k.time;});if(next==curve.keys.end())return curve.keys.back().value;
    auto prev=next-1;float alpha=(time-prev->time)/std::max(next->time-prev->time,1e-6f);return prev->value+(next->value-prev->value)*alpha;
}
inline RootMotionDelta SampleRoot(const std::vector<RootMotionKey>& keys,float time,float duration,bool looping) {
    RootMotionDelta out;if(keys.empty() || duration<=0)return out;float cycles=looping?std::floor(time/duration):0;float t=looping?time-cycles*duration:std::clamp(time,0.f,duration);
    auto next=std::upper_bound(keys.begin(),keys.end(),t,[](float v,const auto& k){return v<k.time;});auto prev=next==keys.begin()?next:next-1;if(next==keys.end())next=prev;
    float alpha=next==prev?0:(t-prev->time)/std::max(next->time-prev->time,1e-6f);
    const auto& first=keys.front();const auto& last=keys.back();
    DirectX::XMFLOAT3 local{prev->position.x+(next->position.x-prev->position.x)*alpha-first.position.x,prev->position.y+(next->position.y-prev->position.y)*alpha-first.position.y,prev->position.z+(next->position.z-prev->position.z)*alpha-first.position.z};
    const double yaw=last.yaw-first.yaw,angle=cycles*yaw;
    double sumCos=cycles,sumSin=0;
    if(std::fabs(std::sin(yaw*.5))>1e-8){const double magnitude=std::sin(cycles*yaw*.5)/std::sin(yaw*.5),mid=(cycles-1)*yaw*.5;sumCos=magnitude*std::cos(mid);sumSin=magnitude*std::sin(mid);}
    const float dx=last.position.x-first.position.x,dz=last.position.z-first.position.z;
    out.translation={float(dx*sumCos+dz*sumSin+local.x*std::cos(angle)+local.z*std::sin(angle)+first.position.x),float(cycles*(last.position.y-first.position.y)+local.y+first.position.y),float(dz*sumCos-dx*sumSin+local.z*std::cos(angle)-local.x*std::sin(angle)+first.position.z)};
    out.yaw=prev->yaw+(next->yaw-prev->yaw)*alpha+float(angle);return out;
}
inline RootMotionDelta RootDelta(const std::vector<RootMotionKey>& keys,float previous,float current,float duration,bool looping) {
    auto a=SampleRoot(keys,previous,duration,looping),b=SampleRoot(keys,current,duration,looping);
    const float dx=b.translation.x-a.translation.x,dz=b.translation.z-a.translation.z,c=std::cos(a.yaw),s=std::sin(a.yaw);
    return {{dx*c-dz*s,b.translation.y-a.translation.y,dx*s+dz*c},b.yaw-a.yaw};
}
// Extract a dense root channel into a cumulative horizontal/yaw track. The
// vertical animation stays on the bone. Existing content opts in explicitly.
inline bool ExtractRootMotion(std::vector<DirectX::XMFLOAT3>& positions,std::vector<DirectX::XMFLOAT4>& rotations,size_t offset,uint32_t frames,float frameRate,float duration,std::vector<RootMotionKey>& keys) {
    using namespace DirectX;if(frames<1 || frameRate<=0 || positions.size()<offset+frames || rotations.size()<offset+frames)return false;
    keys.clear();const auto origin=positions[offset];float firstYaw=0,previousYaw=0,unwrapped=0;
    for(uint32_t f=0;f<frames;++f){auto q=XMLoadFloat4(&rotations[offset+f]);auto forward=XMVector3Rotate(XMVectorSet(0,0,1,0),q);float yaw=std::atan2(XMVectorGetX(forward),XMVectorGetZ(forward));if(f==0){firstYaw=previousYaw=yaw;}else{float d=yaw-previousYaw;while(d>XM_PI)d-=XM_2PI;while(d<-XM_PI)d+=XM_2PI;unwrapped+=d;previousYaw=yaw;}
        keys.push_back({std::min(float(f)/frameRate,duration),{positions[offset+f].x-origin.x,0,positions[offset+f].z-origin.z},unwrapped});
        positions[offset+f].x=origin.x;positions[offset+f].z=origin.z;XMStoreFloat4(&rotations[offset+f],XMQuaternionNormalize(XMQuaternionMultiply(q,XMQuaternionRotationRollPitchYaw(0,firstYaw-yaw,0))));}
    if(keys.back().time<duration){auto end=keys.back();if(keys.size()>1){auto prev=keys[keys.size()-2];float scale=(duration-end.time)/std::max(end.time-prev.time,1e-6f);end.position.x+=(end.position.x-prev.position.x)*scale;end.position.z+=(end.position.z-prev.position.z)*scale;end.yaw+=(end.yaw-prev.yaw)*scale;}end.time=duration;keys.push_back(end);}return true;
}
}
