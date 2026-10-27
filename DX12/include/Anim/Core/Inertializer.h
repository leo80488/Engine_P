#pragma once
#include "Anim/Core/PoseOps.h"
#include <cmath>
#include <algorithm>

namespace Anim {
// Per-bone offsets decay to exactly zero; interrupted blends restart from the
// most recently displayed pose and velocity rather than an old source clip.
class Inertializer {
public:
    bool Active()const{return m_duration>0;}
    void Bind(size_t bones){m_last.resize(bones);m_previous.resize(bones);m_offset.resize(bones);m_velocity.resize(bones);m_target.resize(bones);m_valid=false;m_duration=0;}
    void Seed(const LocalPose* pose,size_t bones){if(m_last.size()!=bones)Bind(bones);std::copy_n(pose,bones,m_last.begin());m_previous=m_last;m_target=m_last;m_valid=true;m_lastDt=0;}
    void Process(LocalPose* pose,size_t bones,float dt,float duration,bool request,bool dead=false) {
        using namespace DirectX;if(m_last.size()!=bones)Bind(bones);
        if(request && m_valid && duration>1e-5f) {
            m_elapsed=0;m_duration=duration;m_dead=dead;
            for(size_t b=0;b<bones;++b) {
                auto difference=[](const XMFLOAT3& a,const XMFLOAT3& c,float scale=1.f){return XMFLOAT3{(a.x-c.x)*scale,(a.y-c.y)*scale,(a.z-c.z)*scale};};
                m_offset[b].pos=difference(m_last[b].pos,pose[b].pos);m_offset[b].scl=difference(m_last[b].scl,pose[b].scl);
                const auto rotation=RotationDelta(m_last[b].rot,pose[b].rot);m_offset[b].rot={rotation.x,rotation.y,rotation.z,0};
                const float inverse=m_lastDt>1e-5f?1.f/m_lastDt:0;
                m_velocity[b].pos=difference(m_last[b].pos,m_previous[b].pos,inverse);
                m_velocity[b].scl=difference(m_last[b].scl,m_previous[b].scl,inverse);
                auto angular=RotationDelta(m_last[b].rot,m_previous[b].rot);
                m_velocity[b].rot={angular.x*inverse,angular.y*inverse,angular.z*inverse,0};
                if(!dead && dt>1e-5f) {
                    const auto incoming=difference(pose[b].pos,m_target[b].pos,1.f/dt);
                    // A discontinuous target is not a velocity estimate.
                    if(std::fabs(incoming.x)+std::fabs(incoming.y)+std::fabs(incoming.z)<100.f) {
                        m_velocity[b].pos.x-=incoming.x;m_velocity[b].pos.y-=incoming.y;m_velocity[b].pos.z-=incoming.z;
                    }
                }
            }
        }else if(request)m_duration=0;
        std::copy_n(pose,bones,m_target.begin());
        if(m_valid && m_duration>0) {
            const float t=std::clamp(m_elapsed/m_duration,0.f,1.f);
            const float decay=1-t*t*t*(10+t*(-15+6*t));
            for(size_t b=0;b<bones;++b) {
                if(m_dead) {
                    LocalPose predicted=m_last[b];predicted.pos.x+=m_velocity[b].pos.x*std::max(dt,0.f);predicted.pos.y+=m_velocity[b].pos.y*std::max(dt,0.f);predicted.pos.z+=m_velocity[b].pos.z*std::max(dt,0.f);
                    const float step=std::max(dt,0.f);predicted.scl.x+=m_velocity[b].scl.x*step;predicted.scl.y+=m_velocity[b].scl.y*step;predicted.scl.z+=m_velocity[b].scl.z*step;
                    const auto spin=Rotation({m_velocity[b].rot.x*step,m_velocity[b].rot.y*step,m_velocity[b].rot.z*step});XMStoreFloat4(&predicted.rot,XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&predicted.rot),XMLoadFloat4(&spin))));
                    pose[b].pos=PoseOps::Lerp3(pose[b].pos,predicted.pos,decay);
                    pose[b].rot=PoseOps::Slerp4(pose[b].rot,predicted.rot,decay);
                    pose[b].scl=PoseOps::Lerp3(pose[b].scl,predicted.scl,decay);
                }else {
                    auto add=[&](XMFLOAT3& value,const XMFLOAT3& offset,const XMFLOAT3& velocity) {
                        value.x+=(offset.x+velocity.x*m_elapsed)*decay;value.y+=(offset.y+velocity.y*m_elapsed)*decay;value.z+=(offset.z+velocity.z*m_elapsed)*decay;
                    };add(pose[b].pos,m_offset[b].pos,m_velocity[b].pos);add(pose[b].scl,m_offset[b].scl,m_velocity[b].scl);
                    const auto& offset=m_offset[b].rot;const auto& velocity=m_velocity[b].rot;
                    const auto rotation=Rotation({(offset.x+velocity.x*m_elapsed)*decay,(offset.y+velocity.y*m_elapsed)*decay,(offset.z+velocity.z*m_elapsed)*decay});
                    XMStoreFloat4(&pose[b].rot,XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&pose[b].rot),XMLoadFloat4(&rotation))));
                }
            }
            m_elapsed+=std::max(dt,0.f);if(t>=1)m_duration=0;
        }
        m_previous.swap(m_last);std::copy_n(pose,bones,m_last.begin());m_lastDt=std::max(dt,0.f);m_valid=true;
    }

private:
    static DirectX::XMFLOAT3 RotationDelta(const DirectX::XMFLOAT4& from,const DirectX::XMFLOAT4& to) {
        using namespace DirectX;
        XMVECTOR q=XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionInverse(XMLoadFloat4(&to)),XMLoadFloat4(&from)));
        XMFLOAT4 value;XMStoreFloat4(&value,q);if(value.w<0){value.x=-value.x;value.y=-value.y;value.z=-value.z;value.w=-value.w;}
        const float length=std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
        const float scale=length>1e-7f?2.f*std::atan2(length,value.w)/length:2.f;
        return {value.x*scale,value.y*scale,value.z*scale};
    }
    static DirectX::XMFLOAT4 Rotation(const DirectX::XMFLOAT3& delta) {
        using namespace DirectX;const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
        XMFLOAT4 result;if(length<=1e-7f)return {0,0,0,1};
        XMStoreFloat4(&result,XMQuaternionRotationAxis(XMVectorSet(delta.x/length,delta.y/length,delta.z/length,0),length));return result;
    }

    std::vector<LocalPose> m_last,m_previous,m_offset,m_velocity,m_target;
    float m_elapsed=0,m_duration=0,m_lastDt=0;
    bool m_valid=false,m_dead=false;
};
}
