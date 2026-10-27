#include "Graphics/AnimProfile.h"
#include "System/TaskSystem.h"
#include "Anim/IK/FootLock.h"
#include "ECS/IKRigSystem.h"
#include "ECS/AnimGraphSystem.h"
#include "Anim/IK/IKRigResolve.h"
#include "Anim/IK/TwoBone.h"
#include "Anim/IK/GroundingMath.h"
#include "Anim/IK/IKGate.h"
#include "Anim/IK/GoalTarget.h"
#include "Anim/IK/GenericSolvers.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "ECS/CharacterControllerComponent.h"
#include "Physics/PhysicsSystem.h"
#include "ECS/HierarchyComponents.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"
using namespace DirectX;
using namespace Anim::IK;
namespace {
float Fade(float value,float target,float seconds,float dt) {
    if(dt<=0 || seconds<=0) return target;
    return value+std::clamp(target-value,-dt/seconds,dt/seconds);
}
XMVECTOR ReadReference(ModelPose& pose,const IKResolvedGoal& b,IKPath path,float ikWeight) {
    if(path==IKPath::IKBoneMMD) {
        const auto original=pose.local[b.ik].pos;
        XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,XMLoadFloat4x4(&pose.skel.restPoseLocal[b.ik]));
        XMStoreFloat3(&pose.local[b.ik].pos,t+(XMLoadFloat3(&original)-t)/std::max(ikWeight,1e-5f));
        pose.RecomputeSubtree(b.ik);auto result=pose.Get(b.ik).r[3];
        pose.local[b.ik].pos=original;pose.RecomputeSubtree(b.ik);return result;
    }
    if(path==IKPath::IKBoneTwoBone) {
        // Reference controls may have a rest offset relative to the effector.
        auto restOffset=XMVector3TransformCoord(XMLoadFloat4x4(&pose.skel.bindPose[b.end]).r[3],
            XMMatrixInverse(nullptr,XMLoadFloat4x4(&pose.skel.bindPose[b.ik])));
        return XMVector3TransformCoord(restOffset,pose.Get(b.ik));
    }
    return pose.Get(b.end).r[3];
}
// The command line is fixed for the process: read each switch once, not per rig or per goal.
bool IKRigOff() { static const bool s_off = Sys::CommandLine::Get().Has("-ikrig_off"); return s_off; }
bool CurvesOff() { static const bool s_off = Sys::CommandLine::Get().Has("-animgraph_curves_off"); return s_off; }
bool FootLockOff() { static const bool s_off = Sys::CommandLine::Get().Has("-ikrig_lock_off"); return s_off; }
}
void IKRigSystem::Prepare(World& world,float dt,DX12Physics::PhysicsSystem* physics,const std::unordered_set<Entity>* active) {
    auto& stats=Anim::AnimProfile::Get();stats.ikRigEntities=stats.ikRigProbes=stats.ikRigSolves=0;
    auto* rigs=world.GetPool<IKRigComponent>();if(!rigs)return;
    auto* skeletons=world.GetPool<SkeletonComponent>();auto* animations=world.GetPool<AnimationComponent>();
    auto* transforms=world.GetPool<GlobalTransform>();
    auto* controllers=world.GetPool<CharacterControllerComponent>();
    for(size_t n=0;n<rigs->Data().size();++n) {
        auto& rig=rigs->Data()[n];Entity entity=rigs->Entities()[n];
        rig.appliedWeights.clear();rig.solvedGoals=0;
        rig.evaluatedThisFrame=false;rig.solveCount=0;
        for(auto& rt:rig.rt) rt.prepared=false;
        if(m_shadows && m_shadows->contains(entity))continue;
        if(!m_enabled || IKRigOff() || !rig.enabled || (active && !active->contains(entity)))continue;
        auto* skeleton=skeletons?skeletons->Get(entity):nullptr;
        auto* local=m_anim.GetMutableLocalPose(entity);
        if(!skeleton || !local || skeleton->assetIndex>=m_skeletons.Count())continue;
        auto& asset=m_skeletons.Get(skeleton->assetIndex);
        if(!asset.humanoid.IsValid() && rig.resolveKey==0)Resource::HumanoidProfileBuilder::Build(asset);
        ResolveRig(asset,rig);
        const bool livePose=m_anim.HasLivePoseSource(world,entity);
        if(!livePose) {
            for(uint32_t b=0;b<asset.boneCount;++b) {
                XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,XMLoadFloat4x4(&asset.restPoseLocal[b]));
                XMStoreFloat3(&local[b].pos,t);XMStoreFloat4(&local[b].rot,r);XMStoreFloat3(&local[b].scl,s);
            }
        }
        auto* transform=transforms?transforms->Get(entity):nullptr;
        auto entityMatrix=transform?XMLoadFloat4x4(&transform->matrix):XMMatrixIdentity();
        if(std::abs(XMVectorGetX(XMMatrixDeterminant(entityMatrix)))<1e-8f)continue;
        const float cameraDistance=XMVectorGetX(XMVector3Length(entityMatrix.r[3]-XMLoadFloat3(&m_camera)));
        if(rig.maxDistance>0 && cameraDistance>rig.maxDistance)continue;
        rig.evaluatedThisFrame=true;++stats.ikRigEntities;++rig.frameCounter;
        XMStoreFloat4x4(&rig.entityMatrix,entityMatrix);
        auto inverse=XMMatrixInverse(nullptr,entityMatrix);
        ModelPose model(local,asset);
        auto* cc=controllers?controllers->Get(entity):nullptr;
        rig.grounded=!cc || cc->isGrounded;
        rig.groundedFade=Fade(rig.groundedFade,rig.grounded?1.f:0.f,rig.ground.airborneFade,dt);
        rig.groundingWeight=Fade(rig.groundingWeight,rig.ground.enabled?1.f:0.f,rig.groundingBlend,dt);
        const float scale=XMVectorGetX(XMVector3Length(entityMatrix.r[1]));
        const float groundWeight=std::clamp(rig.ground.weight*rig.groundedFade*rig.groundingWeight,0.f,1.f);
        const float entityY=XMVectorGetY(entityMatrix.r[3]);
        if(rig.entityInitialized && dt>0) {
            const float jump=entityY-rig.previousEntityY;
            if(rig.grounded && jump>.05f*scale && jump<rig.ground.maxStepUp*scale*1.5f)
                rig.stepOffset=std::max(-rig.ground.pelvisMaxDrop,rig.stepOffset-jump/scale);
        }
        rig.previousEntityY=entityY;rig.entityInitialized=true;
        rig.stepOffset=SmoothGround(rig.stepOffset,0,rig.ground.pelvisSmooth,rig.ground.pelvisSmooth,dt);
        auto* anim=animations?animations->Get(entity):nullptr;
        const float ikWeight=anim?anim->legIKWeight:0;
        for(auto& rt:rig.rt) {rt.gateWeight=1;rt.forceLock=false;}
        if(anim) {
            const auto* graph=m_graphs && m_graphs->OwnsPose(entity)?m_graphs->GetInstance(entity):nullptr;
            if(graph) {
                const Anim::Graph::TickRecord* dominant=nullptr;
                for(const auto& tick:graph->ticks)if(!dominant || tick.weight>dominant->weight)dominant=&tick;
                if(dominant)if(const auto* clip=m_anim.GetClip(dominant->clip)) {
                    float time=dominant->time;
                    if(dominant->looping && clip->duration>0.f){time=std::fmod(time,clip->duration);if(time<0.f)time+=clip->duration;}
                    CollectIKGates(clip->notifyTracks,time,rig);
                }
            } else if(const auto* clip=m_anim.GetClip(anim->primaryClip))CollectIKGates(clip->notifyTracks,anim->primaryTime,rig);
            if(const auto* timeline=world.GetComponent<TimelineComponent>(entity))CollectIKGates(timeline->tracks,anim->primaryTime,rig);
        }
        for(size_t i=0;i<kIKGoalCount;++i) {
            auto& rt=rig.rt[i];const auto& bone=rig.resolved[i];const auto& def=rig.goals[i];auto& script=rig.script[i];
            rt.scriptWeight=Fade(rt.scriptWeight,script.active?std::clamp(script.weight,0.f,1.f):0,script.blend,dt);
            const bool evaluation=m_evaluation && livePose && anim && bone.chain>=0 && anim->IKChainEnabled(bone.chain) && ikWeight>1e-5f;
            auto path=SelectPath(def,bone,evaluation);
            const bool grounding=i<2 && groundWeight>0.f;
            if(path==IKPath::Invalid || (!def.enabled && rt.scriptWeight<=0 && !grounding)) {
                rt.weight=0;rt.path=path;rt.locked=false;rt.lockWeight=0;continue;
            }
            if(path==IKPath::IKBoneMMD && def.refMode==IKRefMode::Virtual && !rt.refWarned) {
                LOG_WARNING("IKRig: Virtual reference conflicts with active MMD evaluation; using IK bone");rt.refWarned=true;
            }
            if(path!=rt.path) {rt.previousPath=rt.path;rt.path=path;rt.pathBlend=rt.initialized?0.f:1.f;}
            rt.pathBlend=Fade(rt.pathBlend,1,.15f,dt);
            auto reference=ReadReference(model,bone,path,ikWeight);
            XMStoreFloat3(&rt.reference,reference);XMStoreFloat3(&rt.target,reference);
            rt.weight=std::clamp(rig.masterWeight*def.weight*rt.gateWeight,0.f,1.f);
            if(def.curveDriven) {
                float curveWeight=1.f;
                if(CurvesOff())curveWeight=0.f;
                else if(!def.curve.empty()) {
                    const auto* instance=m_graphs?m_graphs->GetInstance(entity):nullptr;
                    const auto it=instance?instance->curves.find(def.curve):decltype(instance->curves.find(def.curve)){};
                    curveWeight=instance && m_graphs->OwnsPose(entity) && it!=instance->curves.end()?it->second:0.f;
                }
                rt.weight=std::clamp(rt.weight*curveWeight,0.f,1.f);
                rt.target=def.curveTarget;
                rig.appliedWeights.push_back(rt.weight);
            }
            rt.targetBone=-1;rt.targetHasRotation=script.hasRotation;

            if(grounding && physics) {
                XMFLOAT3 worldRef;XMStoreFloat3(&worldRef,XMVector3TransformCoord(reference,entityMatrix));
                const float startY=std::max(worldRef.y,entityY)+std::max(0.f,rig.ground.probeUp)*scale;
                rt.probeStart={worldRef.x,startY,worldRef.z};
                const float distance=std::max(.001f,(std::max(0.f,rig.ground.probeUp)+std::max(0.f,rig.ground.probeDown))*scale);
                rt.probeEnd={worldRef.x,startY-distance,worldRef.z};
                DX12Physics::ShapeCastHit hit;
                const bool probe=dt<=0 || !rig.groundInitialized || rt.pathBlend<1 ||
                    rig.frameCounter%std::max(1u,rig.probeEveryFrames)==0 ||
                    XMVectorGetX(XMVector3Length(entityMatrix.r[3]-XMLoadFloat3(&rig.previousEntityPosition)))>.1f*scale;
                if(probe) {
                    hit=physics->CastSphereClosest(rt.probeStart,{0,-1,0},
                        std::max(.001f,rig.ground.probeRadius*scale),distance,cc?cc->bodyId:~0u,
                        static_cast<DX12Physics::LayerMask>(rig.ground.layerMask));
                    ++stats.ikRigProbes;
                } else {hit.hit=rt.hit;hit.entity=rt.hitEntity;hit.point=rt.hitPoint;hit.normal=rt.normal;}
                rt.hit=hit.hit;rt.hitEntity=hit.entity;rt.hitPoint=hit.point;
                float rawDelta=0;
                if(hit.hit) {
                    rt.normal=hit.normal;
                    rawDelta=std::clamp((hit.point.y-entityY)/scale,-std::max(0.f,rig.ground.maxStepDown),std::max(0.f,rig.ground.maxStepUp));
                    if(probe && rig.ground.toeProbe && bone.toe>=0) {
                        XMFLOAT3 toe;XMStoreFloat3(&toe,XMVector3TransformCoord(model.Get(bone.toe).r[3],entityMatrix));
                        toe.y=std::max(toe.y,entityY)+rig.ground.probeUp*scale;
                        const auto toeHit=physics->CastSphereClosest(toe,{0,-1,0},std::max(.001f,rig.ground.probeRadius*scale),distance,
                            cc?cc->bodyId:~0u,static_cast<DX12Physics::LayerMask>(rig.ground.layerMask));
                        ++stats.ikRigProbes;
                        if(toeHit.hit)XMStoreFloat3(&rt.normal,GroundNormal(XMLoadFloat3(&hit.point),XMLoadFloat3(&toeHit.point),XMLoadFloat3(&hit.normal)));
                    }
                }
                rt.groundDelta=SmoothGround(rt.groundDelta,rawDelta,rig.ground.smoothUp,rig.ground.smoothDown,rig.groundInitialized?dt:0);
                const float sole=rig.ground.soleOffset<0
                    ? std::max(0.f,asset.bindPose[bone.end]._42):rig.ground.soleOffset;
                const auto goal=GroundTarget(reference,entityMatrix,inverse,rt.groundDelta*scale,
                    hit.point.y,sole*scale,hit.hit);
                XMStoreFloat3(&rt.target,XMVectorLerp(reference,goal,groundWeight));
                const auto worldGoal=XMVector3TransformCoord(XMLoadFloat3(&rt.target),entityMatrix);
                const float rootSpeed=rt.initialized && dt>0
                    ? XMVectorGetX(XMVector3Length(reference-XMLoadFloat3(&rt.previousRootPosition)))/dt:1e30f;
                const float height=XMVectorGetY(worldGoal)-hit.point.y-sole*scale;
                const float entityMove=rig.entityInitialized?XMVectorGetX(XMVector3Length(entityMatrix.r[3]-XMLoadFloat3(&rig.previousEntityPosition)))/scale:0;
                UpdateFootLock(rt,rig.ground,worldGoal,rootSpeed,height,scale,entityMove,dt,rig.grounded,!FootLockOff());
                if(rt.lockWeight>0)XMStoreFloat3(&rt.target,XMVectorLerp(XMLoadFloat3(&rt.target),XMVector3TransformCoord(XMLoadFloat3(&rt.anchor),inverse),rt.lockWeight));
            } else {rt.groundDelta=0;rt.hit=false;rt.locked=false;rt.lockWeight=Fade(rt.lockWeight,0,rig.ground.lockRelease,dt);}
            XMStoreFloat3(&rt.previousRootPosition,reference);
            if(script.space==IKTargetSpace::World || script.space==IKTargetSpace::EntityLocal) {
                auto target=XMLoadFloat3(&script.position);
                if(script.space==IKTargetSpace::World) target=XMVector3TransformCoord(target,inverse);
                XMStoreFloat3(&rt.target,XMVectorLerp(XMLoadFloat3(&rt.target),target,rt.scriptWeight));
                rt.targetRotation=script.rotation;
                if(script.hasRotation && script.space==IKTargetSpace::World) {
                    XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,entityMatrix);
                    XMStoreFloat4(&rt.targetRotation,XMQuaternionMultiply(XMLoadFloat4(&script.rotation),XMQuaternionInverse(r)));
                }
            } else if(script.space==IKTargetSpace::Entity) {
                const Entity target=script.entity.Resolve(world);
                const auto* gt=target?world.GetComponent<GlobalTransform>(target):nullptr;
                bool valid=gt!=nullptr;
                auto source=gt?XMLoadFloat4x4(&gt->matrix):XMMatrixIdentity();
                if(!script.socket.empty()) {
                    const auto* sockets=target?world.GetComponent<SocketComponent>(target):nullptr;
                    const auto index=sockets?sockets->FindByName(SocketComponent::HashName(script.socket.c_str())):~0u;
                    valid=sockets && index<sockets->count;
                    if(valid)source=XMLoadFloat4x4(&sockets->sockets[index].worldTransform);
                }
                if(valid)XMStoreFloat4x4(&rt.lastExternalFrame,GoalFrame(source,XMLoadFloat3(&script.position),XMLoadFloat4(&script.rotation)));
                if(valid)rt.externalFrameValid=true;
                else {script.active=false;rt.scriptWeight=Fade(rt.scriptWeight,0,script.blend,dt);}
                if(rt.externalFrameValid) {
                    const auto frame=XMLoadFloat4x4(&rt.lastExternalFrame)*inverse;
                    XMStoreFloat3(&rt.target,XMVectorLerp(XMLoadFloat3(&rt.target),frame.r[3],rt.scriptWeight));
                    XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,frame);
                    XMStoreFloat4(&rt.targetRotation,r);rt.targetHasRotation=true;
                }
            } else if(script.space==IKTargetSpace::SelfBone) {
                const auto* sockets=world.GetComponent<SocketComponent>(entity);
                const auto index=sockets?sockets->FindByName(SocketComponent::HashName(script.socket.c_str())):~0u;
                auto sourceOffset=XMMatrixIdentity();
                if(sockets && index<sockets->count) {
                    rt.targetBone=int32_t(sockets->sockets[index].boneIndex);
                    sourceOffset=XMLoadFloat4x4(&sockets->sockets[index].localOffset);
                } else rt.targetBone=script.socket=="@RightHand"?rig.resolved[3].end:FindBone(asset,script.socket);
                XMStoreFloat4x4(&rt.targetFrame,GoalFrame(sourceOffset,XMLoadFloat3(&script.position),XMLoadFloat4(&script.rotation)));
                if(rt.targetBone<0 || uint32_t(rt.targetBone)>=asset.boneCount) {script.active=false;rt.targetBone=-1;}
                else rt.targetHasRotation=true;
            }
            auto pole=bone.mid>=0?model.Get(bone.mid).r[3]:reference;
            const auto root=bone.root>=0?model.Get(bone.root).r[3]:reference;
            const auto axis=XMVector3Normalize(reference-root);
            const auto bend=pole-root-axis*XMVector3Dot(pole-root,axis);
            if(def.poleMode==IKPoleMode::CharacterForward || XMVectorGetX(XMVector3LengthSq(bend))<1e-8f) {
                auto forward=bone.toe>=0?model.Get(bone.toe).r[3]-reference:XMVectorSet(0,0,1,0);
                if(XMVectorGetX(XMVector3LengthSq(forward))<1e-8f)forward=XMVectorSet(0,0,1,0);
                pole=root+XMVector3Normalize(forward)*(i<2?1.f:-1.f);
            }
            if(def.poleMode==IKPoleMode::Bone && bone.pole>=0)pole=model.Get(bone.pole).r[3];
            if(def.poleMode==IKPoleMode::WorldPoint)pole=def.curveDriven
                ?XMLoadFloat3(&def.polePoint):XMVector3TransformCoord(XMLoadFloat3(&def.polePoint),inverse);
            XMStoreFloat3(&rt.pole,pole);
            XMStoreFloat3(&rt.virtualTarget,XMLoadFloat3(&rt.target)+(model.Get(bone.end).r[3]-reference)*(1.f-rt.scriptWeight));
            rt.prepared=true;
        }
        rig.groundInitialized=true;
        XMStoreFloat3(&rig.previousEntityPosition,entityMatrix.r[3]);
        rig.preparedPose.assign(local,local+asset.boneCount);
        // References were captured before the pelvis moves; targets remain in
        // that frame so lowering the hips actually bends the legs.
        const int32_t hips=asset.humanoid.hipsTranslationBone>=0?asset.humanoid.hipsTranslationBone:asset.humanoid.Bone(HumanoidBone::Hips);
        const float drop=rig.ground.pelvisAdjust && groundWeight>0
            ? PelvisDrop(rig.rt[0].groundDelta*rig.rt[0].gateWeight,rig.rt[1].groundDelta*rig.rt[1].gateWeight,rig.ground.pelvisMaxDrop)+rig.stepOffset:0;
        rig.pelvisOffset=SmoothGround(rig.pelvisOffset,drop,rig.ground.pelvisSmooth,rig.ground.pelvisSmooth,dt);
        if(hips>=0 && uint32_t(hips)<asset.boneCount && asset.parentIndex[hips]>=0) {
            auto displacement=XMVector3TransformNormal(XMVectorSet(0,rig.pelvisOffset*scale*groundWeight*rig.masterWeight,0,0),inverse);
            displacement=XMVector3TransformNormal(displacement,XMMatrixInverse(nullptr,model.Get(asset.parentIndex[hips])));
            XMStoreFloat3(&local[hips].pos,XMLoadFloat3(&local[hips].pos)+displacement);model.RecomputeSubtree(hips);
        }
        for(size_t i=0;i<kIKGoalCount;++i) {
            auto& rt=rig.rt[i];const auto& b=rig.resolved[i];
            if(!rt.prepared || rt.path!=IKPath::IKBoneMMD)continue;
            rt.ikOriginalPosition=local[b.ik].pos;
            auto goal=XMVectorLerp(XMLoadFloat3(&rt.reference),XMLoadFloat3(&rt.target),rt.weight);
            auto parent=asset.parentIndex[b.ik];
            if(parent>=0)goal=XMVector3TransformCoord(goal,XMMatrixInverse(nullptr,model.Get(parent)));
            XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,XMLoadFloat4x4(&asset.restPoseLocal[b.ik]));
            XMStoreFloat3(&local[b.ik].pos,t+(goal-t)*ikWeight);
            model.RecomputeSubtree(b.ik);
        }
    }
}
void IKRigSystem::Solve(World& world,float,const std::unordered_set<Entity>* active) {
    if(!m_enabled || IKRigOff())return;
    auto* rigs=world.GetPool<IKRigComponent>();auto* skeletons=world.GetPool<SkeletonComponent>();
    if(!rigs || !skeletons)return;
    struct Job {IKRigComponent* rig;AnimationSystem::LocalPose* local;const SkeletonAsset* asset;const AnimationComponent* animation;};
    std::vector<Job> jobs;
    const auto* animations=world.GetPool<AnimationComponent>();
    for(size_t n=0;n<rigs->Data().size();++n) {
        auto& rig=rigs->Data()[n];auto e=rigs->Entities()[n];
        if(!rig.evaluatedThisFrame || (active && !active->contains(e)))continue;
        const auto* skel=skeletons->Get(e);auto* local=m_anim.GetMutableLocalPose(e);
        if(!skel || !local || skel->assetIndex>=m_skeletons.Count())continue;
        jobs.push_back({&rig,local,&m_skeletons.Get(skel->assetIndex),animations?animations->Get(e):nullptr});
    }
    auto solve=[&](uint32_t index) {
        const auto& job=jobs[index];auto& rig=*job.rig;auto* local=job.local;
        ModelPose model(local,*job.asset);const auto* animation=job.animation;
        // The primary grip (right hand) is solved before the secondary hand,
        // which may read its bone/socket on this very frame.
        for(size_t i: {size_t(0),size_t(1),size_t(3),size_t(2),size_t(4),size_t(5),size_t(6),size_t(7)}) {
            auto& rt=rig.rt[i];auto& b=rig.resolved[i];const auto& def=rig.goals[i];
            if(!rt.prepared)continue;
            ++rig.solveCount;
            if(rt.targetBone>=0) {
                if(IsAncestor(model.skel,b.root,rt.targetBone) || rt.targetBone==b.end) {
                    ENGINE_CHECK_ONCE(false,"SelfBone IK target depends on its own driven limb");continue;
                }
                const auto frame=XMLoadFloat4x4(&rt.targetFrame)*model.Get(rt.targetBone);
                XMStoreFloat3(&rt.target,XMVectorLerp(XMLoadFloat3(&rt.target),frame.r[3],rt.scriptWeight));
                XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,frame);XMStoreFloat4(&rt.targetRotation,r);
            }
            if(rt.path==IKPath::IKBoneMMD) {
                local[b.ik].pos=rt.ikOriginalPosition;model.RecomputeSubtree(b.ik);
                if(animation && animation->legIKWeight<1.f && b.root>=0 && b.mid>=0) {
                    // Solve the FK side separately, then blend with the already
                    // evaluated MMD result. Grounding stays continuous at zero.
                    auto fk=rig.preparedPose;
                    for(uint32_t bone=0;bone<model.skel.boneCount;++bone)fk[bone].pos=local[bone].pos;
                    ModelPose virtualModel(fk.data(),model.skel);
                    if(SolveTwoBone(virtualModel,b.root,b.mid,b.end,XMLoadFloat3(&rt.virtualTarget),XMLoadFloat3(&rt.pole),def.softness)) {
                        for(auto bone:{b.root,b.mid,b.end}) {
                            auto corrected=XMQuaternionSlerp(XMLoadFloat4(&rig.preparedPose[bone].rot),XMLoadFloat4(&fk[bone].rot),rt.weight);
                            XMStoreFloat4(&local[bone].rot,XMQuaternionSlerp(corrected,XMLoadFloat4(&local[bone].rot),animation->legIKWeight));
                        }
                        model.RecomputeSubtree(b.root);
                    }
                }
            } else if(rt.path==IKPath::Generic || rt.path==IKPath::LookAt) {
                const auto bones=rt.path==IKPath::LookAt?std::vector<uint32_t>{uint32_t(b.end)}:b.links;
                std::vector<XMFLOAT4> base;for(auto bone:bones)base.push_back(local[bone].rot);
                bool solved=rt.path==IKPath::LookAt
                    ? SolveLookAt(model,b.end,XMLoadFloat3(&rt.target),XMLoadFloat3(&def.lookAxis),def.lookLimitDeg)
                    : def.solver==IKSolverKind::FABRIK ? SolveFABRIK(model,b.links,XMLoadFloat3(&rt.target)) : SolveCCD(model,b.links,XMLoadFloat3(&rt.target));
                if(!ENGINE_CHECK_ONCE(solved,"Invalid generic IK goal"))continue;
                for(size_t j=0;j<bones.size();++j)XMStoreFloat4(&local[bones[j]].rot,XMQuaternionSlerp(XMLoadFloat4(&base[j]),XMLoadFloat4(&local[bones[j]].rot),rt.weight));
                model.RecomputeSubtree(bones.front());
            } else {
                std::array<uint32_t,3> bones={uint32_t(b.root),uint32_t(b.mid),uint32_t(b.end)};
                std::array<XMFLOAT4,3> base;
                for(size_t j=0;j<3;++j)base[j]=local[bones[j]].rot;
                if(!ENGINE_CHECK_ONCE(SolveTwoBone(model,b.root,b.mid,b.end,XMLoadFloat3(&rt.target),XMLoadFloat3(&rt.pole),def.softness),"Invalid two-bone IK input"))continue;
                for(size_t j=0;j<3;++j) {
                    auto rotation=XMQuaternionSlerp(XMLoadFloat4(&base[j]),XMLoadFloat4(&local[bones[j]].rot),rt.weight);
                    if(rt.pathBlend<1 && rt.initialized)rotation=XMQuaternionSlerp(XMLoadFloat4(&rt.previousRotations[j]),rotation,rt.pathBlend);
                    XMStoreFloat4(&local[bones[j]].rot,rotation);
                }
                model.RecomputeSubtree(b.root);
            }
            if(def.handRot==IKHandRot::MatchTarget && rt.targetHasRotation && rt.scriptWeight>0) {
                XMVECTOR scale,current,t;XMMatrixDecompose(&scale,&current,&t,model.Get(b.end));
                auto desired=XMQuaternionSlerp(current,XMLoadFloat4(&rt.targetRotation),rt.scriptWeight*rt.weight);
                model.ApplyModelRotationDelta(b.end,XMQuaternionMultiply(XMQuaternionInverse(current),desired));
            }
            if(i<2 && rig.ground.alignToGround && rt.hit && rt.weight>0) {
                const auto footWorld=model.Get(b.end)*XMLoadFloat4x4(&rig.entityMatrix);
                const auto localSoleUp=XMVector3TransformNormal(XMVectorSet(0,1,0,0),
                    XMLoadFloat4x4(&model.skel.inverseBindPose[b.end]));
                auto alignment=GroundAlignment(XMLoadFloat3(&rt.normal),rig.ground.maxAlignDeg,
                    XMVector3Normalize(XMVector3TransformNormal(localSoleUp,footWorld)));
                alignment=XMQuaternionSlerp(XMQuaternionIdentity(),alignment,
                    rt.weight*rig.ground.weight*rig.groundedFade*rig.groundingWeight);
                XMVECTOR scale,entityRot,t;
                XMMatrixDecompose(&scale,&entityRot,&t,XMLoadFloat4x4(&rig.entityMatrix));
                auto modelAlignment=XMQuaternionMultiply(XMQuaternionMultiply(entityRot,alignment),XMQuaternionInverse(entityRot));
                model.ApplyModelRotationDelta(b.end,modelAlignment);
            }
            auto effector=model.Get(b.end).r[3];
            rt.error=XMVectorGetX(XMVector3Length(XMVector3TransformNormal(effector-XMLoadFloat3(&rt.target),XMLoadFloat4x4(&rig.entityMatrix))));
            rt.reached=rt.error<.005f;rt.initialized=true;
            if(def.curveDriven && rt.weight>0.f)++rig.solvedGoals;
            // Also seed from MMD output, so crossing to virtual IK never
            // interpolates from an uninitialized quaternion.
            if(b.root>=0 && b.mid>=0) {
                const int32_t bones[]={b.root,b.mid,b.end};
                for(size_t j=0;j<3;++j)rt.previousRotations[j]=local[bones[j]].rot;
            }
            XMStoreFloat3(&rt.effector,XMVector3TransformCoord(effector,XMLoadFloat4x4(&rig.entityMatrix)));
        }
    };
    // The math baseline is ~6 us for a 100-bone FK+limb in Release. Small
    // batches stay serial; larger batches amortize dispatch across characters.
    if(jobs.size()>4)TaskSystem::Get().ParallelFor(0,uint32_t(jobs.size()),solve);
    else for(uint32_t i=0;i<jobs.size();++i)solve(i);
    for(const auto& job:jobs)Anim::AnimProfile::Get().ikRigSolves+=job.rig->solveCount;
}

void IKRigSystem::DrawDebug(World& world,DebugWirePass& draw) {
    auto* rigs=world.GetPool<IKRigComponent>();if(!rigs)return;
    for(const auto& rig:rigs->Data())if(rig.debugDraw && rig.evaluatedThisFrame) {
        const auto entity=XMLoadFloat4x4(&rig.entityMatrix);
        XMFLOAT3 origin,pelvis;
        XMStoreFloat3(&origin,entity.r[3]);
        XMStoreFloat3(&pelvis,entity.r[3]+XMVectorSet(0,rig.pelvisOffset*XMVectorGetX(XMVector3Length(entity.r[1])),0,0));
        draw.AddLine(origin,pelvis,0xffffaa00);draw.AddCross(pelvis,.035f,0xffffaa00);
        for(const auto& rt:rig.rt)if(rt.prepared) {
        XMFLOAT3 ref,target,pole;
        auto worldMatrix=XMLoadFloat4x4(&rig.entityMatrix);
        XMStoreFloat3(&ref,XMVector3TransformCoord(XMLoadFloat3(&rt.reference),worldMatrix));
        XMStoreFloat3(&target,XMVector3TransformCoord(XMLoadFloat3(&rt.target),worldMatrix));
        XMStoreFloat3(&pole,XMVector3TransformCoord(XMLoadFloat3(&rt.pole),worldMatrix));
        draw.AddSphere(ref,.025f,0xffffff00);draw.AddCross(target,.04f,0xff00ff00);
        draw.AddLine(rt.effector,target,0xff00ffff);draw.AddLine(ref,pole,0xffff00ff);
        if(rt.lockWeight>0)draw.AddSphere(rt.anchor,.035f,0xffffaa00);
        draw.AddLine(rt.probeStart,rt.probeEnd,0xff00aaff);
        if(rt.hit) {
            draw.AddCross(rt.hitPoint,.03f,0xffffffff);
            XMFLOAT3 normalEnd;XMStoreFloat3(&normalEnd,XMLoadFloat3(&rt.hitPoint)+XMLoadFloat3(&rt.normal)*.2f);
            draw.AddLine(rt.hitPoint,normalEnd,0xff00ff00);
        }
        }
    }
}
