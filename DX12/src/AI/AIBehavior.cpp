#include "AI/AIBehavior.h"
#include "ECS/PerceptionComponent.h"
#include "ECS/HierarchyComponents.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace AI {
namespace {
using DirectX::XMFLOAT3;
XMFLOAT3 Position(const World& w,Entity e) {
    if(auto* t=w.GetComponent<GlobalTransform>(e))return {t->matrix._41,t->matrix._42,t->matrix._43};
    return {};
}
float Distance(const XMFLOAT3& a,const XMFLOAT3& b) {
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return std::sqrt(x*x+y*y+z*z);
}
template<class T>T Read(const BlackboardComponent& bb,const char* key,T fallback) {
    if(auto* v=bb.Get<T>(key))return *v;return fallback;
}
bool Eligible(World& w,Entity self,Entity target,const AIBehaviorComponent& config) {
    if(target==NullEntity||target==self||!w.IsAlive(target)||!w.GetComponent<GlobalTransform>(target))return false;
    auto* passive=w.GetComponent<PerceptibleComponent>(target);
    if(!passive||!passive->enabled)return false;
    if(config.targetMode==3)return false;
    if(config.targetMode==2)return target==config.fixedTarget.Resolve(w);
    auto* perception=w.GetComponent<PerceptionComponent>(self);
    return perception&&passive->faction!=perception->faction;
}
void ClearTarget(BlackboardComponent& bb) {
    bb.Erase("TargetEntity");bb.Erase("TargetGeneration");
    bb.Set("HasTarget",false);bb.Set("TargetVisible",false);
}
void Contact(World& w,Entity self,Entity target,BlackboardComponent& bb,float now) {
    bb.Set("TargetEntity",target);bb.Set("TargetGeneration",int(w.GetGeneration(target)));
    bb.Set("LastKnownPosition",Position(w,target));bb.Set("LastContactTime",now);
    bb.Set("HasTarget",true);bb.Set("AIState",std::string("chase"));
    bb.Erase("PatrolWaitUntil");bb.Erase("PatrolLegStart");bb.Erase("PatrolGoal");
}
}

Entity BehaviorTarget(World& w,Entity e) {
    auto* bb=w.GetComponent<BlackboardComponent>(e);auto* config=w.GetComponent<AIBehaviorComponent>(e);
    if(!bb||!config||!config->enabled)return NullEntity;
    Entity target=Read<Entity>(*bb,"TargetEntity",NullEntity);
    if(!Eligible(w,e,target,*config)||w.GetGeneration(target)!=uint32_t(Read<int>(*bb,"TargetGeneration",-1)))return NullEntity;
    return target;
}

void ResetBehavior(World& w,Entity e) {
    if(auto* bb=w.GetComponent<BlackboardComponent>(e))
        for(const char* key:{"TargetEntity","TargetGeneration","HasTarget","TargetVisible","LastKnownPosition",
            "LastContactTime","AIState","HomePosition","PatrolIndex","PatrolDirection","PatrolWaitUntil",
            "PatrolLegStart","PatrolGoal","PatrolWaiting","PatrolError","BehaviorTime","PatrolFinished"})bb->Erase(key);
}

bool AlertBehavior(World& w,Entity e,Entity instigator) {
    auto* config=w.GetComponent<AIBehaviorComponent>(e);auto* bb=w.GetComponent<BlackboardComponent>(e);
    if(!config||!config->enabled||!bb||!Eligible(w,e,instigator,*config))return false;
    Contact(w,e,instigator,*bb,Read<float>(*bb,"BehaviorTime",0));
    if(auto* ai=w.GetComponent<AIComponent>(e))ai->wakeRequested=true;
    return true;
}

void UpdateBehavior(World& w,Entity e,float now) {
    auto* config=w.GetComponent<AIBehaviorComponent>(e);
    if(!config){if(auto* bb=w.GetComponent<BlackboardComponent>(e);bb&&bb->Has("BehaviorTime"))ResetBehavior(w,e);return;}
    if(!w.GetComponent<BlackboardComponent>(e))w.AddComponent<BlackboardComponent>(e,{});
    auto& bb=*w.GetComponent<BlackboardComponent>(e);
    bb.Set("BehaviorTime",now);bb.Erase("PatrolGoal");bb.Set("PatrolWaiting",false);
    if(!config->enabled){ClearTarget(bb);bb.Set("AIState",std::string("disabled"));return;}
    const auto position=Position(w,e);
    if(!bb.Has("HomePosition"))bb.Set("HomePosition",position);
    const auto home=Read<XMFLOAT3>(bb,"HomePosition",position);
    const bool wasCombat=Read<bool>(bb,"HasTarget",false);
    Entity current=BehaviorTarget(w,e),chosen=NullEntity;
    auto* perception=w.GetComponent<PerceptionComponent>(e);
    auto withinLeash=[&](Entity target){return config->leashRadius<=0||
        (Distance(position,home)<=config->leashRadius&&Distance(Position(w,target),home)<=config->leashRadius);};
    float best=std::numeric_limits<float>::max();
    if(perception)for(const auto& seen:perception->visibleEntities) {
        if(!Eligible(w,e,seen.entity,*config)||seen.generation!=w.GetGeneration(seen.entity)||!withinLeash(seen.entity))continue;
        if(config->targetMode==1&&seen.entity==current){chosen=current;break;}
        const float distance=Distance(position,Position(w,seen.entity));
        if(distance<best||(distance==best&&seen.entity<chosen)){chosen=seen.entity;best=distance;}
    }
    if(config->targetMode==2&&!config->fixedRequiresSight) {
        Entity fixed=config->fixedTarget.Resolve(w);
        if(Eligible(w,e,fixed,*config)&&withinLeash(fixed))chosen=fixed;
    }
    bb.Set("TargetVisible",false);
    if(chosen!=NullEntity) {
        Contact(w,e,chosen,bb,now);
        bool visible=false;
        if(perception)for(const auto& v:perception->visibleEntities)
            if(v.entity==chosen&&v.generation==w.GetGeneration(chosen)){visible=true;break;}
        bb.Set("TargetVisible",visible);return;
    }
    if(current!=NullEntity&&withinLeash(current)&&now-Read<float>(bb,"LastContactTime",-1e10f)<std::max(0.f,config->memorySeconds)) {
        bb.Set("HasTarget",true);bb.Set("AIState",std::string("chase"));
        if(config->trackDuringMemory)bb.Set("LastKnownPosition",Position(w,current));
        return;
    }
    ClearTarget(bb);
    if(wasCombat){bb.Set("AIState",std::string("return"));bb.Erase("PatrolLegStart");bb.Erase("PatrolWaitUntil");bb.Erase("PatrolFinished");}
    if(!bb.Has("AIState"))bb.Set("AIState",std::string("patrol"));

    std::vector<XMFLOAT3> points;
    for(const auto& ref:config->patrolPoints) {
        Entity point=ref.Resolve(w);
        if(point==NullEntity||!w.GetComponent<GlobalTransform>(point)) {
            bb.Set("PatrolError",std::string("Missing patrol point: repair the route in AI Behavior"));return;
        }
        points.push_back(Position(w,point));
    }
    bb.Erase("PatrolError");
    const bool returning=Read<std::string>(bb,"AIState","")=="return";
    if(points.empty()) {
        if(returning&&Distance(position,home)>config->arriveRadius)bb.Set("PatrolGoal",home);
        else bb.Set("AIState",std::string("idle"));
        return;
    }
    int index=std::clamp(Read<int>(bb,"PatrolIndex",1)-1,0,int(points.size())-1);
    if(returning) {
        float nearest=std::numeric_limits<float>::max();
        for(int i=0;i<int(points.size());++i)if(float d=Distance(position,points[i]);d<nearest){nearest=d;index=i;}
    }
    bb.Set("PatrolIndex",index+1);
    if(Read<bool>(bb,"PatrolFinished",false)&&config->patrolMode==2){bb.Set("PatrolWaiting",true);return;}
    if(now<Read<float>(bb,"PatrolWaitUntil",-1.f)){bb.Set("PatrolWaiting",true);return;}
    if(!bb.Has("PatrolLegStart"))bb.Set("PatrolLegStart",now);
    if(Distance(position,points[index])<=std::max(.01f,config->arriveRadius)||
        now-Read<float>(bb,"PatrolLegStart",now)>=std::max(.1f,config->legTimeout)) {
        int direction=Read<int>(bb,"PatrolDirection",1);
        if(config->patrolMode==2&&index==int(points.size())-1)bb.Set("PatrolFinished",true);
        else if(points.size()>1) {
            if(config->patrolMode==1){if(index==int(points.size())-1)direction=-1;else if(index==0)direction=1;index+=direction;}
            else index=(index+1)%int(points.size());
        }
        bb.Set("PatrolIndex",index+1);bb.Set("PatrolDirection",direction);
        bb.Set("PatrolWaitUntil",now+std::max(0.f,config->waitSeconds));bb.Erase("PatrolLegStart");
        bb.Set("PatrolWaiting",true);bb.Set("AIState",std::string("patrol"));
    } else {bb.Set("PatrolGoal",points[index]);if(!returning)bb.Set("AIState",std::string("patrol"));}
}
}
