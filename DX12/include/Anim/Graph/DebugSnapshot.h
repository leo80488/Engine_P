#pragma once
#include "Anim/Graph/AnimGraphInstance.h"
#include <nlohmann/json.hpp>
namespace Anim::Graph {
inline nlohmann::json DebugSnapshot(const Instance& instance) {
    using Json=nlohmann::json;
    Json nodes=Json::array(),samples=Json::array(),parameters=Json::object(),events=Json::array(),tasks=Json::array(),requests=Json::array();
    for(size_t i=0;i<instance.states.size();++i) {
        const auto& state=instance.states[i];const auto& node=instance.definition->nodes[i];
        Json item{{"id",node.id},{"weight",state.weight},{"time",state.time},{"relevant",state.relevant},{"task",state.task}};
        if(node.type==NodeType::StateMachine && state.active>=0 && state.active<node.states.size()) {
            item["state"]=node.states[state.active].name;item["stateTime"]=state.stateTime;
            const int source=state.source>=0?state.source:state.departing;item["source"]=source>=0?node.states[source].name:"";item["target"]=node.states[state.active].name;
            item["transitioning"]=state.Transitioning();item["progress"]=!state.Transitioning()?1.f:std::clamp(state.elapsed/std::max(state.duration,1e-3f),0.f,1.f);
        }nodes.push_back(std::move(item));
    }
    for(const auto& tick:instance.ticks)samples.push_back({{"node",instance.definition->nodes[tick.node].id},{"clip",tick.clip},{"weight",tick.weight},{"time",tick.time},{"rate",tick.rate}});
    for(size_t i=0;i<instance.definition->params.size();++i) {
        const auto& declaration=instance.definition->params[i];const auto& value=instance.params[i];Json item;
        if(declaration.type==ParamType::Float)item=value.Float();
        else if(declaration.type==ParamType::Vector3)item=value.vector;
        else if(declaration.type==ParamType::Bool || declaration.type==ParamType::Trigger)item=value.Bool();
        else item=static_cast<int32_t>(value.integer);
        parameters[declaration.name]=std::move(item);
    }
    static const char* kStatusNames[]={"pending","playing","blendingOut","interrupted","finished","rejected"};
    for(const auto& request:instance.requests)if(request.handle.id)requests.push_back({{"id",request.handle.id},{"generation",request.handle.generation},{"slot",request.desc.slot},{"alias",request.desc.alias},{"status",kStatusNames[static_cast<int>(request.status)]},{"time",request.time},{"weight",request.weight}});
    for(const auto& event:instance.debugEvents)events.push_back(event);
    for(const auto& task:instance.tasks)tasks.push_back({{"op",static_cast<int>(task.op)},{"a",task.a},{"b",task.b},{"clip",task.clip},{"alpha",task.alpha},{"time",task.time}});
    return {{"frame",instance.frame},{"generation",instance.generation},{"definitionHash",std::to_string(instance.definition->contentHash)},
        {"slotRequests",requests},{"nodes",nodes},{"samples",samples},{"parameters",parameters},{"events",events},{"tasks",tasks},
        {"timeScale",instance.timeScale},{"timeScaleRemaining",instance.timeScaleRemaining},
        {"rootMotion",{{"translation",{instance.rootMotion.translation.x,instance.rootMotion.translation.y,instance.rootMotion.translation.z}},{"yaw",instance.rootMotion.yaw}}},{"curves",instance.curves},{"lod",instance.lod},{"skippedFrames",instance.skippedFrames},{"poseHash",instance.poseHash},{"maxAngularDelta",instance.maxAngularDelta},{"poseBuffersHighWater",instance.executor.HighWater()},{"legIKWeight",instance.legIKWeight},{"evaluationUs",instance.evaluationUs}};
}
}
