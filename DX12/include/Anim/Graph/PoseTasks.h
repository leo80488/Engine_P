#pragma once
#include "Anim/Graph/AnimGraphDefinition.h"
#include "Anim/Core/Inertializer.h"

namespace Anim::Graph {
enum class TaskOp : uint8_t { RefPose, Sample, Blend, Additive, Copy, SnapshotLoad, Inertialize };
struct PoseTask {
    TaskOp op = TaskOp::RefPose;
    int a = -1, b = -1, mask = -1;
    uint32_t clip = kInvalidClip;
    float time = 0, alpha = 0;
    int reference = -1;
    std::vector<LocalPose>* snapshot = nullptr;
    Anim::Inertializer* inertializer = nullptr;
    float dt=0,duration=0;
    bool inertialRequest=false,deadBlend=false,meshSpace=false,activeA=true;
    bool looping=true;
};
// A liveness-managed pool: DAG fan-out retains a buffer until its last reader.
// Storage is retained between frames; each evaluator owns its scratch.
class PoseExecutor {
public:
    bool Execute(const std::vector<PoseTask>&, int root, const BoundGraph&,
                 const ClipLibrary&, LocalPose* out);
    size_t HighWater() const { return m_highWater; }
private:
    size_t m_highWater = 0;

};
}
