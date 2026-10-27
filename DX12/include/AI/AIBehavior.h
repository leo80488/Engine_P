#pragma once
#include "AI/AIComponents.h"
#include "ECS/GuidComponent.h"

// Authored scene/prefab data. Working memory belongs to BlackboardComponent.
struct AIBehaviorComponent {
    bool enabled=true;
    // 0 nearest visible hostile, 1 retain current hostile, 2 fixed target, 3 none.
    int targetMode=1;
    AttachmentRef fixedTarget;
    bool fixedRequiresSight=true;
    float memorySeconds=5.f;
    float leashRadius=30.f; // 0 disables the leash; measured from HomePosition.
    bool trackDuringMemory=false;
    // 0 loop, 1 ping-pong, 2 once. Empty route means stay at home.
    int patrolMode=0;
    std::vector<AttachmentRef> patrolPoints;
    float arriveRadius=.4f,waitSeconds=1.5f,legTimeout=10.f;
};

namespace AI {
void UpdateBehavior(World& world,Entity entity,float now);
Entity BehaviorTarget(World& world,Entity entity);
bool AlertBehavior(World& world,Entity entity,Entity instigator);
void ResetBehavior(World& world,Entity entity);
}
