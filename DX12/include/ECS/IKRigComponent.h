#pragma once
#include "ECS/GuidComponent.h"
#include "Anim/IK/LocalPose.h"
#include <array>
#include <string>
#include <vector>

enum class IKGoalSlot : uint8_t { LeftFoot,RightFoot,LeftHand,RightHand,Custom0,Custom1,Custom2,Custom3,Count };
enum class IKRefMode : uint8_t { Auto,IKBone,Virtual };
enum class IKSolverKind : uint8_t { Auto,MmdChain,TwoBone,CCD,FABRIK,LookAt };
enum class IKPoleMode : uint8_t { Animated,CharacterForward,Bone,WorldPoint };
enum class IKHandRot : uint8_t { KeepAnimated,MatchTarget };
enum class IKPath : uint8_t { Invalid,IKBoneMMD,IKBoneTwoBone,VirtualTwoBone,Generic,LookAt };
inline constexpr size_t kIKGoalCount=size_t(IKGoalSlot::Count);
inline constexpr const char* kIKGoalNames[]={"LeftFoot","RightFoot","LeftHand","RightHand","Custom0","Custom1","Custom2","Custom3"};
inline const char* IKPathName(IKPath p) {
    switch(p) {
    case IKPath::IKBoneMMD:return "IKBone+MMD";
    case IKPath::IKBoneTwoBone:return "IKBone+TwoBone";
    case IKPath::VirtualTwoBone:return "Virtual+TwoBone";
    case IKPath::Generic:return "Generic";
    case IKPath::LookAt:return "LookAt";
    default:return "Invalid";
    }
}
struct IKGoalDef {
    bool enabled=false; float weight=1.f;
    IKRefMode refMode=IKRefMode::Auto; IKSolverKind solver=IKSolverKind::Auto;
    std::string ikBone,rootBone,midBone,endBone;
    // Compatibility with AnimGraph.IKGoal and authored curve-driven goals.
    std::string curve;
    DirectX::XMFLOAT3 curveTarget={0,0,0};
    bool curveDriven=false;
    IKPoleMode poleMode=IKPoleMode::Animated; std::string poleBone;
    DirectX::XMFLOAT3 polePoint={0,0,1};
    float softness=0.05f; IKHandRot handRot=IKHandRot::KeepAnimated;
    DirectX::XMFLOAT3 lookAxis={0,0,1};float lookLimitDeg=80.f;
};
struct IKGroundingDef {
    bool enabled=false; float weight=1.f;
    float probeUp=.5f,probeDown=.8f,probeRadius=.04f,soleOffset=-1.f;
    float maxStepUp=.45f,maxStepDown=.45f;
    bool pelvisAdjust=true; float pelvisMaxDrop=.35f;
    float smoothUp=18.f,smoothDown=10.f,pelvisSmooth=8.f;
    bool alignToGround=true; float maxAlignDeg=30.f; bool toeProbe=true;
    bool footLock=false; float lockSpeed=.15f,lockHeight=.04f,lockMaxDrift=.25f,lockRelease=.12f;
    float airborneFade=.10f;
    uint32_t layerMask=3; // StaticEnv | DynamicProp; checked against physics constants
};
struct IKResolvedGoal {
    int32_t root=-1,mid=-1,end=-1,ik=-1,chain=-1,toe=-1,pole=-1;
    std::vector<uint32_t> links;
};
enum class IKTargetSpace : uint8_t { None,World,EntityLocal,Entity,SelfBone };
struct IKScriptTarget {
    IKTargetSpace space=IKTargetSpace::None;
    DirectX::XMFLOAT3 position={0,0,0}; DirectX::XMFLOAT4 rotation={0,0,0,1};
    bool hasRotation=false; float weight=1.f,blend=.2f;
    bool active=false;
    AttachmentRef entity; std::string socket;
};
struct IKGoalRuntime {
    IKPath path=IKPath::Invalid,previousPath=IKPath::Invalid;
    bool prepared=false,reached=false,locked=false,hit=false,initialized=false;
    float error=0,weight=0,scriptWeight=0,groundDelta=0,lockWeight=0,pathBlend=1;
    float gateWeight=1; bool forceLock=false;
    Entity hitEntity=NullEntity;
    DirectX::XMFLOAT3 reference={0,0,0},target={0,0,0},effector={0,0,0},pole={0,0,1};
    DirectX::XMFLOAT3 virtualTarget={0,0,0};
    DirectX::XMFLOAT3 normal={0,1,0},anchor={0,0,0},previousRootPosition={0,0,0};
    DirectX::XMFLOAT3 probeStart={0,0,0},probeEnd={0,0,0},hitPoint={0,0,0};
    DirectX::XMFLOAT4 targetRotation={0,0,0,1};
    bool targetHasRotation=false; int32_t targetBone=-1;
    DirectX::XMFLOAT4X4 targetFrame;
    DirectX::XMFLOAT4X4 lastExternalFrame;
    bool externalFrameValid=false,refWarned=false;
    std::array<DirectX::XMFLOAT4,3> previousRotations{};
    DirectX::XMFLOAT3 ikOriginalPosition={0,0,0};
};
struct IKRigComponent {
    bool enabled=true,debugDraw=false; float masterWeight=1.f;
    float maxDistance=50.f; uint32_t probeEveryFrames=1;
    IKGroundingDef ground;
    std::array<IKGoalDef,kIKGoalCount> goals;
    // Runtime only: never written by the serializer.
    std::array<IKResolvedGoal,kIKGoalCount> resolved;
    std::array<IKGoalRuntime,kIKGoalCount> rt;
    std::array<IKScriptTarget,kIKGoalCount> script;
    uint64_t resolveKey=0;
    uint32_t frameCounter=0,solveCount=0; bool evaluatedThisFrame=false;
    float pelvisOffset=0,groundedFade=1,groundingWeight=1,groundingBlend=.1f;
    bool grounded=true,groundInitialized=false,entityInitialized=false;
    float previousEntityY=0,stepOffset=0;
    DirectX::XMFLOAT3 previousEntityPosition={0,0,0};
    DirectX::XMFLOAT4X4 entityMatrix;
    std::vector<Anim::IK::LocalPose> preparedPose;
    std::vector<float> appliedWeights;
    uint32_t solvedGoals=0;
};
template<> struct ComponentCloner<IKRigComponent> {
    static IKRigComponent Clone(const IKRigComponent& src) {
        IKRigComponent copy; copy.enabled=src.enabled; copy.debugDraw=src.debugDraw;
        copy.masterWeight=src.masterWeight; copy.ground=src.ground; copy.goals=src.goals;
        copy.maxDistance=src.maxDistance; copy.probeEveryFrames=src.probeEveryFrames;
        return copy;
    }
};
