#pragma once
#include "ECS/IKRigComponent.h"
#include "ECS/NotifyTypes.h"
#include <algorithm>
namespace Anim::IK {
inline void CollectIKGates(const std::vector<NotifyTrack>& tracks,float time,IKRigComponent& rig) {
    for(const auto& track:tracks)if(!track.muted)for(const auto& state:track.states) {
        if(state.category!=NotifyCategory::IKGate || time<state.startTime || time>=state.endTime)continue;
        const unsigned mask=unsigned(state.params.GetInt("goalMask",3));
        const float weight=std::clamp(state.params.GetFloat("weight",0.f),0.f,1.f);
        for(size_t i=0;i<kIKGoalCount;++i)if(mask&(1u<<i)) {
            rig.rt[i].gateWeight*=weight;
            rig.rt[i].forceLock |= state.params.GetInt("forceLock",0)!=0;
        }
    }
}
}
