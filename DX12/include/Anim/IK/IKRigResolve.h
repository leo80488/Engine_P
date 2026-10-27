#pragma once
#include "ECS/IKRigComponent.h"
#include "Resource/SkeletonAsset.h"
#include <cstring>

namespace Anim::IK {
inline int32_t FindBone(const SkeletonAsset& s,const std::string& name) {
    if(name.empty()) return -1;
    for(uint32_t i=0;i<s.boneCount;++i) if(name==s.boneNames[i]) return int32_t(i);
    return -1;
}
inline bool IsAncestor(const SkeletonAsset& s,int32_t parent,int32_t child) {
    if(parent<0 || child<0 || uint32_t(child)>=s.boneCount) return false;
    for(int32_t i=s.parentIndex[child];i>=0 && i<child;child=i,i=s.parentIndex[i]) if(i==parent) return true;
    return false;
}
inline void ResolveRig(const SkeletonAsset& s,IKRigComponent& rig) {
    uint64_t key=14695981039346656037ull;
    auto hash=[&](const std::string& text) { for(unsigned char c:text) key=(key^c)*1099511628211ull; key=(key^0xff)*1099511628211ull; };
    for(uint32_t i=0;i<s.boneCount;++i) { hash(s.boneNames[i]); key=(key^uint32_t(s.parentIndex[i]))*1099511628211ull; }
    for(auto b:s.humanoid.boneForRole) key=(key^uint32_t(b))*1099511628211ull;
    for(const auto& chain:s.ikChains) key=(key^chain.ikBoneIndex^chain.targetBoneIndex)*1099511628211ull;
    for(const auto& g:rig.goals) { hash(g.rootBone);hash(g.midBone);hash(g.endBone);hash(g.ikBone);hash(g.poleBone);key=(key^uint8_t(g.solver))*1099511628211ull; }
    if(key==rig.resolveKey) return;
    rig.resolveKey=key;
    static constexpr HumanoidBone kRoles[4][4]={
        {HumanoidBone::LeftUpperLeg,HumanoidBone::LeftLowerLeg,HumanoidBone::LeftFoot,HumanoidBone::LeftToes},
        {HumanoidBone::RightUpperLeg,HumanoidBone::RightLowerLeg,HumanoidBone::RightFoot,HumanoidBone::RightToes},
        {HumanoidBone::LeftUpperArm,HumanoidBone::LeftLowerArm,HumanoidBone::LeftHand,HumanoidBone::Count},
        {HumanoidBone::RightUpperArm,HumanoidBone::RightLowerArm,HumanoidBone::RightHand,HumanoidBone::Count}};
    static const char* kControls[]={"ik_foot_l","ik_foot_r","ik_hand_l","ik_hand_r"};
    for(size_t i=0;i<kIKGoalCount;++i) {
        const auto& d=rig.goals[i]; auto& g=rig.resolved[i];g={};
        auto role=[&](const std::string& name,size_t r) {
            return name.empty() && i<4 ? (kRoles[i][r]==HumanoidBone::Count ? -1:s.humanoid.Bone(kRoles[i][r])) : FindBone(s,name);
        };
        g.root=role(d.rootBone,0);g.mid=role(d.midBone,1);g.end=role(d.endBone,2);g.toe=role({},3);
        g.pole=FindBone(s,d.poleBone);g.ik=FindBone(s,d.ikBone);
        for(uint32_t c=0;c<s.ikChains.size();++c) {
            const auto& chain=s.ikChains[c];
            if((g.ik>=0 && uint32_t(g.ik)==chain.ikBoneIndex) ||
                (g.ik<0 && g.end>=0 && uint32_t(g.end)==chain.targetBoneIndex)) {
                g.chain=int32_t(c);g.ik=int32_t(chain.ikBoneIndex);
                if(d.endBone.empty())g.end=int32_t(chain.targetBoneIndex);
                if(d.midBone.empty() && g.mid<0 && !chain.links.empty())g.mid=int32_t(chain.links.front().boneIndex);
                if(d.rootBone.empty() && g.root<0 && chain.links.size()>1)g.root=int32_t(chain.links.back().boneIndex);
                break;
            }
        }
        if(g.ik<0 && d.ikBone.empty() && i<4) g.ik=FindBone(s,kControls[i]);
        if(d.solver==IKSolverKind::CCD || d.solver==IKSolverKind::FABRIK) {
            if(IsAncestor(s,g.root,g.end)) {
                for(int32_t bone=g.end;bone>=g.root;bone=s.parentIndex[bone]) {
                    g.links.push_back(uint32_t(bone));if(bone==g.root)break;
                }
                std::reverse(g.links.begin(),g.links.end());
                const bool grants=std::any_of(g.links.begin(),g.links.end(),[&](auto bone){return s.grantSource[bone]>=0 && s.grantRatio[bone]!=0;});
                if(grants)g.links.clear();
            }
        } else if(d.solver!=IKSolverKind::LookAt && (!IsAncestor(s,g.root,g.mid) || !IsAncestor(s,g.mid,g.end))) { g.root=-1;g.mid=-1; }
        for(auto bone:{g.root,g.mid,g.end})if(bone>=0 && s.grantSource[bone]>=0 && s.grantRatio[bone]!=0) {g.root=-1;g.mid=-1;g.end=-1;}
        rig.rt[i]={};
    }
}
inline IKPath SelectPath(const IKGoalDef& d,const IKResolvedGoal& g,bool evaluation) {
    if(g.end<0) return IKPath::Invalid;
    if(d.refMode==IKRefMode::IKBone && g.ik<0)return IKPath::Invalid;
    if(d.solver==IKSolverKind::MmdChain && (g.chain<0 || !evaluation))return IKPath::Invalid;
    if(d.solver==IKSolverKind::LookAt)return IKPath::LookAt;
    if(d.solver==IKSolverKind::CCD || d.solver==IKSolverKind::FABRIK)return g.links.empty()?IKPath::Invalid:IKPath::Generic;
    if(g.chain>=0 && evaluation) return IKPath::IKBoneMMD;
    if(g.root<0 || g.mid<0) return IKPath::Invalid;
    if(g.ik>=0 && g.chain<0 && d.refMode!=IKRefMode::Virtual) return IKPath::IKBoneTwoBone;
    return IKPath::VirtualTwoBone;
}
}
