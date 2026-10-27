#pragma once
#include "ECS/IKRigComponent.h"

// One authored-field list shared by scene I/O and editor controls. Runtime
// caches, targets, physics results and clocks deliberately stay outside it.
template<class Visitor> void VisitIKRigAuthoring(IKRigComponent& rig,Visitor&& visit) {
    const IKRigComponent defaults{};
    visit("enabled",rig.enabled,defaults.enabled);
    visit("weight",rig.masterWeight,defaults.masterWeight);
    visit("debug",rig.debugDraw,defaults.debugDraw);
    visit("maxDistance",rig.maxDistance,defaults.maxDistance);
    visit("probeEveryFrames",rig.probeEveryFrames,defaults.probeEveryFrames);
#define IK_GROUND(name) visit("ground." #name,rig.ground.name,defaults.ground.name)
    IK_GROUND(enabled);IK_GROUND(weight);IK_GROUND(probeUp);IK_GROUND(probeDown);IK_GROUND(probeRadius);IK_GROUND(soleOffset);
    IK_GROUND(maxStepUp);IK_GROUND(maxStepDown);IK_GROUND(pelvisAdjust);IK_GROUND(pelvisMaxDrop);
    IK_GROUND(smoothUp);IK_GROUND(smoothDown);IK_GROUND(pelvisSmooth);IK_GROUND(alignToGround);IK_GROUND(maxAlignDeg);IK_GROUND(toeProbe);
    IK_GROUND(footLock);IK_GROUND(lockSpeed);IK_GROUND(lockHeight);IK_GROUND(lockMaxDrift);IK_GROUND(lockRelease);IK_GROUND(airborneFade);IK_GROUND(layerMask);
#undef IK_GROUND
    for(size_t i=0;i<kIKGoalCount;++i) {
        const std::string prefix=std::string(kIKGoalNames[i])+".";
#define IK_GOAL(name) visit((prefix+#name).c_str(),rig.goals[i].name,defaults.goals[i].name)
        IK_GOAL(enabled);IK_GOAL(weight);IK_GOAL(refMode);IK_GOAL(solver);
        IK_GOAL(rootBone);IK_GOAL(midBone);IK_GOAL(endBone);IK_GOAL(ikBone);
        IK_GOAL(poleMode);IK_GOAL(poleBone);IK_GOAL(polePoint.x);IK_GOAL(polePoint.y);IK_GOAL(polePoint.z);
        IK_GOAL(softness);IK_GOAL(handRot);
        IK_GOAL(lookAxis.x);IK_GOAL(lookAxis.y);IK_GOAL(lookAxis.z);IK_GOAL(lookLimitDeg);
        IK_GOAL(curveDriven);IK_GOAL(curve);IK_GOAL(curveTarget.x);IK_GOAL(curveTarget.y);IK_GOAL(curveTarget.z);
#undef IK_GOAL
    }
}
