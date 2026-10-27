#pragma once
#include "ECS/NotifyTypes.h"
#include "ECS/AnimFXGateComponent.h"
namespace Anim::NotifyDispatch {
void DispatchPoint(World&, Entity, const Notify&);
void DispatchStateBegin(World&, Entity, const NotifyState&);
void DispatchStateEnd(World&, Entity, const NotifyState&);
void CollectFXGates(const std::vector<NotifyTrack>&, float, ActiveFXGates&);
}
