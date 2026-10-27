#pragma once
#include "ECS/IKRigComponent.h"
#include "Anim/IK/TwoBone.h"
namespace Anim::IK {
inline void UpdateFootLock(IKGoalRuntime& rt,const IKGroundingDef& ground,
    DirectX::FXMVECTOR worldGoal,float speed,float height,float scale,float entityMove,
    float dt,bool grounded,bool allow) {
    using namespace DirectX;
    const bool canLock=dt>0 && grounded && rt.hit && rt.gateWeight>0 && ground.footLock && allow;
    const float drift=XMVectorGetX(XMVector3Length(worldGoal-XMLoadFloat3(&rt.anchor)))/scale;
    if(!canLock || (rt.locked && (drift>ground.lockMaxDrift || entityMove>ground.lockMaxDrift*4)))rt.locked=false;
    if(canLock && !rt.locked && rt.lockWeight<=0 && (rt.forceLock || (speed<ground.lockSpeed && std::abs(height)<ground.lockHeight))) {
        rt.locked=true;XMStoreFloat3(&rt.anchor,worldGoal);
    }
    if(dt<=0)rt.lockWeight=0;
    else {
        const float seconds=rt.locked?.04f:ground.lockRelease;
        const float target=rt.locked?1.f:0.f;
        rt.lockWeight=seconds<=0?target:rt.lockWeight+std::clamp(target-rt.lockWeight,-dt/seconds,dt/seconds);
    }
}
}
