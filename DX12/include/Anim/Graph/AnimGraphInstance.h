#pragma once
#include "Anim/Graph/PoseTasks.h"
#include "Anim/Core/ClipTracks.h"
#include <unordered_map>
#include "ECS/LocomotionComponents.h"
#include "Anim/Graph/NotifySampler.h"
#include <deque>
#include <unordered_set>
#include <span>
#include <cstddef>

namespace Anim::Graph {
struct TickRecord {
    NodeIndex node;
    uint32_t clip;
    int task;
    float weight, rate;
    bool looping;
    bool manual = false;
    float time = 0, previous = 0;
    uint32_t player = 0;
    bool keepSampleTime=false,leader=true,activeState=true;
    float phaseOffset=0;
    NodeIndex eventOwner=kNoNode;
};
class Instance {
public:
    struct AliasSettings { bool looping=false; float rate=1; };

    struct Event { NodeIndex source; std::string name; float weight = 1;std::string from,to,mode; };

    struct SyncState{float phase=0,previous=0;bool initialized=false;};

    struct SlotRecord {
        SlotHandle handle;
        SlotRequestDesc desc;
        SlotStatus status = SlotStatus::Rejected;
        float time = 0, weight = 0, elapsed = 0, pendingTime = 0;
        bool stopped = false;
        float previousTime = 0, outStartWeight = 1;
        bool authoredClock = false, authoredEnd = false;
    };

    struct Snapshot {
        uint64_t definitionHash=0;
        uint32_t generation=0, frame=0, seed=1, poseHash=0;
        float timeScale=1, timeScaleRemaining=0, timeScaleRestore=1, dt=0, legIKWeight=0, maxAngularDelta=0;
        std::vector<NodeState> states;
        std::array<ParamValue,64> params{};
        std::unordered_map<std::string,uint32_t> slots;
        std::unordered_map<std::string,AliasSettings> aliases;
        std::array<SlotRecord,32> requests{};
        std::vector<LocomotionComponent> spaces;
        std::vector<std::vector<LocalPose>> poses;
        std::vector<Anim::Inertializer> inertializers;
        Anim::Inertializer rootInertial;
        NotifySampler notifies;
        std::array<float,64> ttl{};
        uint64_t triggers=0, consumed=0;
        std::unordered_set<std::string> previousEvents;
        std::deque<std::string> debugEvents;
        std::vector<LocalPose> output;
        bool resume=false;
        std::unordered_map<std::string,SyncState> syncStates;
        Anim::RootMotionDelta rootMotion;
        std::unordered_map<std::string,float> curves;
        std::array<ParamValue,64> previousParams{};
        float carry=0;uint32_t lodCounter=0,skippedFrames=0;
    };
    struct RecordedFrame {
        float dt=0, timeScale=1, timeScaleRemaining=0,timeScaleRestore=1;
        uint64_t triggers=0;
        std::array<float,64> ttl{};
        std::array<ParamValue,64> params{};
        std::array<SlotRecord,32> requests{};
        std::unordered_map<std::string,uint32_t> slots;
        std::unordered_map<std::string,AliasSettings> aliases;
        std::array<float,16> world{};
        std::vector<PoseTask> tasks;
        uint32_t poseHash=0;
            bool inertialEnabled=true,markerSyncEnabled=true,lodEnabled=true,rootMotionEnabled=true,relevanceEndEnabled=true,pendingQueueEnabled=true;
        int lod=0,updateInterval=1;
        std::shared_ptr<const Snapshot> checkpoint;
    };

    Instance(std::shared_ptr<const Definition>, std::shared_ptr<const BoundGraph>, uint32_t seed = 1);
    ~Instance();
    Instance(const Instance&)=delete;
    Instance& operator=(const Instance&)=delete;
    bool Evaluate(const ClipLibrary&, float dt, LocalPose* out);

    const LocomotionComponent* GetBlendSpace(NodeIndex index)const{return index<states.size()?BlendSpace(index):nullptr;}

    bool SetTimeScale(float scale,float duration=0);

    void ResumeFrom(const LocalPose* pose){m_rootInertial.Seed(pose,bound->rest.size());lastOutput.assign(pose,pose+bound->rest.size());m_resume=true;}
    void EndNotifies() { m_notifySampler.EndAll(notifyEvents); }

    Snapshot Capture() const;
    bool Restore(const Snapshot&);
    void StartRecording(size_t capacity=600){m_recorded.clear();m_recordCapacity=std::clamp(capacity,size_t(1),size_t(36000));m_recording=true;}
    void StopRecording(){m_recording=false;}
    bool Recording()const{return m_recording;}
    const std::vector<RecordedFrame>& RecordedFrames()const{return m_recorded;}
    bool Rewind(const ClipLibrary&,size_t frame);

    bool Request(SlotHandle, const SlotRequestDesc&);
    bool Stop(SlotHandle, float blendOut);
    SlotRecord* FindRequest(SlotHandle);
    void Latch(const std::array<ParamValue, 64>&, uint64_t triggers);

private:
    LocomotionComponent* BlendSpace(size_t index)const;

    void Consume(NodeIndex);
    void ResetBranch(NodeIndex);
    void SetPhase(NodeIndex,float);
    float NormalizedTime(NodeIndex) const;
    bool Looping(NodeIndex) const;
    ParamValue Value(NodeIndex);
    int Pose(NodeIndex, float weight, float rate = 1);
    int AddTask(PoseTask task) { tasks.push_back(task); return static_cast<int>(tasks.size() - 1); }

public:
    std::shared_ptr<const Definition> definition;
    std::shared_ptr<const BoundGraph> bound;

private:
    std::unique_ptr<std::byte[]> m_blob;

public:
    std::span<NodeState> states;

    std::array<ParamValue, 64> params{};
    std::unordered_map<std::string, uint32_t> slots;

    std::unordered_map<std::string, AliasSettings> aliasSettings;
    std::vector<PoseTask> tasks;
    std::vector<TickRecord> ticks;
    float legIKWeight = 0, timeScale = 1;
    float timeScaleRemaining=0,timeScaleRestore=1;

    uint32_t frame = 0;
    PoseExecutor executor;
    uint32_t generation = 1;

    std::vector<Event> events;
    std::vector<SampledNotify> notifyEvents;
    std::deque<std::string> debugEvents;
    float evaluationUs = 0, maxAngularDelta = 0;
    uint32_t poseHash = 0;
    std::vector<LocalPose> lastOutput;
    bool inertialEnabled=true,markerSyncEnabled=true,lodEnabled=true,rootMotionEnabled=true,relevanceEndEnabled=true,pendingQueueEnabled=true;
    int lod=0,updateInterval=1;
    uint32_t skippedFrames=0;
    Anim::RootMotionDelta rootMotion;
    std::unordered_map<std::string,float> curves;

    std::unordered_map<std::string,SyncState> syncStates;

    std::array<SlotRecord, 32> requests{};

    bool holdRecordedPose=false;
    std::array<float,16> recordWorld{};

private:
    const ClipLibrary* m_clips = nullptr;
    float m_dt = 0,m_lodCarry=0;
    uint32_t m_lodCounter=0;
    std::array<ParamValue,64> m_previousParams{};
    uint32_t m_seed = 1;

    std::vector<std::vector<LocalPose>> m_snapshots;
    std::array<float, 64> m_triggerTtl{};
    uint64_t m_triggers = 0, m_consumed = 0;
    NotifySampler m_notifySampler;
    std::unordered_set<std::string> m_previousEvents;
    std::vector<Anim::Inertializer> m_inertializers;
    Anim::Inertializer m_rootInertial;
    bool m_resume=false;
    bool m_recording=false,m_replaying=false;
    size_t m_recordCapacity=600;
    std::vector<RecordedFrame> m_recorded;
};
}
