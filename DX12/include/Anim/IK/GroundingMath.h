#pragma once
#include "Anim/IK/TwoBone.h"
namespace Anim::IK {
inline DirectX::XMVECTOR GroundNormal(DirectX::FXMVECTOR heel,DirectX::FXMVECTOR toe,
                                     DirectX::FXMVECTOR fallback) {
    using namespace DirectX;
    const auto forward=toe-heel;
    if(XMVectorGetX(XMVector3LengthSq(forward))<1e-8f)return fallback;
    if(Finite3(fallback) && XMVectorGetX(XMVector3LengthSq(fallback))>1e-8f) {
        const auto direction=XMVector3Normalize(forward);
        // Heel/toe measure longitudinal slope; retain the contact normal's
        // lateral bank rather than replacing it with world-up.
        const auto fitted=fallback-direction*XMVector3Dot(fallback,direction);
        if(XMVectorGetX(XMVector3LengthSq(fitted))>1e-8f) {
            const auto normal=XMVector3Normalize(fitted);
            return XMVectorGetY(normal)>=0?normal:-normal;
        }
    }
    auto lateral=XMVector3Cross(XMVectorSet(0,1,0,0),forward);
    if(XMVectorGetX(XMVector3LengthSq(lateral))<1e-8f)return fallback;
    auto normal=XMVector3Normalize(XMVector3Cross(forward,lateral));
    return XMVectorGetY(normal)>=0?normal:-normal;
}
inline DirectX::XMVECTOR GroundAlignment(DirectX::FXMVECTOR normal,float maxDegrees,
    DirectX::FXMVECTOR up=DirectX::XMVectorSet(0,1,0,0)) {
    using namespace DirectX;
    if(!Finite3(normal) || XMVectorGetX(XMVector3LengthSq(normal))<1e-8f)return XMQuaternionIdentity();
    auto rotation=FromTo(up,normal);
    const float angle=2.f*std::acos(std::clamp(XMVectorGetW(rotation),0.f,1.f));
    const float limit=XMConvertToRadians(std::max(0.f,maxDegrees));
    return angle>limit && angle>1e-5f ? XMQuaternionSlerp(XMQuaternionIdentity(),rotation,limit/angle):rotation;
}
inline float SmoothGround(float current,float target,float up,float down,float dt) {
    if(dt<=0)return target;
    const float rate=target>current?up:down;
    return current+(target-current)*(1.f-std::exp(-std::max(rate,0.f)*dt));
}
inline float PelvisDrop(float left,float right,float maximum) {
    return std::clamp(std::min({left,right,0.f}),-std::max(maximum,0.f),0.f);
}
inline DirectX::XMVECTOR GroundTarget(DirectX::FXMVECTOR reference,
    DirectX::FXMMATRIX entity,DirectX::CXMMATRIX inverse,float delta,
    float groundY,float soleWorld,bool hit)
{
    using namespace DirectX;
    auto world=XMVector3TransformCoord(reference,entity);
    world=XMVectorSetY(world,XMVectorGetY(world)+delta);
    if(hit)world=XMVectorSetY(world,std::max(XMVectorGetY(world),groundY+soleWorld));
    return XMVector3TransformCoord(world,inverse);
}
}
