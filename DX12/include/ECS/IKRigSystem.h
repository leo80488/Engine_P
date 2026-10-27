#pragma once
#include "ECS/AnimationSystem.h"
#include "ECS/IKRigComponent.h"
namespace DX12Physics { class PhysicsSystem; }
class DebugWirePass;
class AnimGraphSystem;
class IKRigSystem {
public:
    IKRigSystem(AnimationSystem& anim,SkeletonRegistry& skeletons):m_anim(anim),m_skeletons(skeletons) {}
    void Prepare(World&,float dt,DX12Physics::PhysicsSystem*,const std::unordered_set<Entity>* active=nullptr);
    void Solve(World&,float dt,const std::unordered_set<Entity>* active=nullptr);
    void DrawDebug(World&,DebugWirePass&);
    bool IsEnabled()const{return m_enabled;} void SetEnabled(bool value){m_enabled=value;}
    void SetRelevance(const DirectX::XMFLOAT3& camera,const std::unordered_set<Entity>* shadows){m_camera=camera;m_shadows=shadows;}
    void SetEvaluationEnabled(bool enabled){m_evaluation=enabled;}
    void SetGraphSystem(const AnimGraphSystem* value){m_graphs=value;}
private:
    const AnimGraphSystem* m_graphs=nullptr;
    AnimationSystem& m_anim; SkeletonRegistry& m_skeletons; bool m_enabled=true;
    DirectX::XMFLOAT3 m_camera={0,0,0}; const std::unordered_set<Entity>* m_shadows=nullptr;bool m_evaluation=true;
};
