#pragma once
#include "Anim/Core/PoseOps.h"
#include "ECS/ECS.h"

namespace Anim {
struct IAnimPoseSource {
    virtual ~IAnimPoseSource() = default;
    virtual bool OwnsPose(Entity) const = 0;
    virtual bool EvaluatePose(Entity, const SkeletonAsset&, LocalPose*, uint32_t,
                              float dt) = 0;
    virtual void CaptureExternalPose(Entity, const LocalPose*, uint32_t) {}
    virtual void Reset() = 0;
};
}
