#include "Anim/Graph/AnimGraphInstance.h"
#include "Resource/SkeletonAsset.h"
#include "Anim/Graph/AnimGraphCompiler.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Anim/Graph/BlendSpaceGeometry.h"
#include "Anim/Graph/MarkerSync.h"

namespace Anim::Graph {
namespace {
std::shared_ptr<const Definition> WithLayout(std::shared_ptr<const Definition> def){
    if(def->instanceLayout.stateOffsets.size()==def->nodes.size())return def;
    auto compiled=std::make_shared<Definition>(*def);compiled->instanceLayout=BuildInstanceLayout(*compiled);return compiled;
}
}
LocomotionComponent* Instance::BlendSpace(size_t index)const{
    const auto offset=definition->instanceLayout.blendOffsets[index];
    return offset==InstanceLayout::NoOffset?nullptr:reinterpret_cast<LocomotionComponent*>(m_blob.get()+offset);
}
Instance::~Instance(){
    for(size_t i=0;i<states.size();++i)if(auto* space=BlendSpace(i))std::destroy_at(space);
    std::destroy_n(states.data(),states.size());
}
Instance::Instance(std::shared_ptr<const Definition> def, std::shared_ptr<const BoundGraph> binding, uint32_t seed)
    : definition(WithLayout(std::move(def))), bound(std::move(binding)),
      m_blob(std::make_unique<std::byte[]>(definition->instanceLayout.bytes)),
      states(reinterpret_cast<NodeState*>(m_blob.get()),definition->nodes.size()), m_seed(seed ? seed : 1) {
    static_assert(sizeof(NodeState)%alignof(NodeState)==0);
    std::uninitialized_value_construct_n(states.data(),states.size());
    for(size_t i=0;i<states.size();++i)if(BlendSpace(i))std::construct_at(BlendSpace(i));
    for (size_t i = 0; i < definition->params.size() && i < params.size(); ++i) params[i] = definition->params[i].value;
    for (size_t i = 0; i < states.size(); ++i) states[i].time = states[i].sampleTime = definition->nodes[i].startTime;
    tasks.reserve(states.size() * 2); ticks.reserve(states.size());
    lastOutput=bound->rest;
    m_snapshots.resize(states.size());
    m_inertializers.resize(states.size());m_rootInertial.Bind(bound->rest.size());
    for (size_t i = 0; i < states.size(); ++i) {
        const auto& node = definition->nodes[i];
        if(node.type==NodeType::StateMachine || node.type==NodeType::Inertialization)m_inertializers[i].Bind(bound->rest.size());
        if (node.type == NodeType::StateMachine) m_snapshots[i] = bound->rest;
        if (node.type != NodeType::BlendSpace1D && node.type != NodeType::BlendSpace2D) continue;
        auto* space=BlendSpace(i);
        space->mode = node.type == NodeType::BlendSpace1D ? 0 : 1;
        space->damping = node.damping;
        space->sampleCount = static_cast<int>(node.samples.size());
        for (size_t p = 0; p < node.samples.size(); ++p) {
            space->samples[p].speed = node.samples[p].speed; space->samples[p].angleDeg = node.samples[p].angle;
        }
    }
}
ParamValue Instance::Value(NodeIndex index) {
    if (index == kNoNode || index >= states.size()) return {};
    auto& st = states[index];
    if (st.valueFrame == frame) return st.value;
    st.valueFrame = frame;
    st.weight = 1;
    const auto& n = definition->nodes[index];
    if(n.type==NodeType::Select){st.value=Value(n.input[Value(n.input[2]).Bool()?1:0]);return st.value;}
    const auto a = Value(n.input[0]), b = Value(n.input[1]);
    switch (n.type) {
    case NodeType::Param: st.value = n.param >= 0 && n.param < 64 ? params[n.param] : ParamValue{}; break;
    case NodeType::Constant: st.value = n.constant; break;
    case NodeType::Not: st.value = ParamValue::FromInt(!a.Bool()); break;
    case NodeType::Compare: {
        const auto typeA=definition->nodes[n.input[0]].valueType,typeB=definition->nodes[n.input[1]].valueType;
        auto numeric=[](const ParamValue& value,ParamType type){return type==ParamType::Int?double(static_cast<int32_t>(value.integer)):double(value.Float());};
        bool result=false;
        if(typeA==ParamType::Id || typeA==ParamType::Bool || typeA==ParamType::Trigger || typeA==ParamType::Vector3) {
            const bool equal=typeA==ParamType::Vector3?a.vector==b.vector:a.integer==b.integer;
            result=n.operation=="notEqual"?!equal:equal;
        }else {
            const double x=numeric(a,typeA),y=numeric(b,typeB);
            result=n.operation=="less"?x<y:n.operation=="equal"?x==y:n.operation=="notEqual"?x!=y:n.operation=="lessEqual"?x<=y:n.operation=="greaterEqual"?x>=y:x>y;
        }
        st.value = ParamValue::FromInt(result); break;
    }
    case NodeType::And: st.value = ParamValue::FromInt(a.Bool() && b.Bool()); break;
    case NodeType::Or: st.value = ParamValue::FromInt(a.Bool() || b.Bool()); break;
    case NodeType::Xor: st.value = ParamValue::FromInt(a.Bool() != b.Bool()); break;
    case NodeType::Select: st.value = Value(n.input[Value(n.input[2]).Bool() ? 1 : 0]); break;
    case NodeType::Remap: {
        const float low=n.input[1]==kNoNode?n.inputMin:b.Float(),high=n.input[2]==kNoNode?n.inputMax:Value(n.input[2]).Float();
        const float to=n.input[3]==kNoNode?n.outputMax:Value(n.input[3]).Float();
        float t=std::fabs(high-low)>1e-6f?(a.Float()-low)/(high-low):0;if(n.clampRemap)t=std::clamp(t,0.f,1.f);
        st.value=ParamValue::FromFloat(n.outputMin+(to-n.outputMin)*t);break;
    }
    case NodeType::ClipCurve: {float v=0;if(n.reference<states.size()){const auto& player=definition->nodes[n.reference];auto clip=slots.find(player.slot);if(clip!=slots.end() && clip->second<m_clips->Count()){const auto& asset=m_clips->Get(clip->second);float t=states[n.reference].time;if(player.looping && asset.duration>0){t=std::fmod(t,asset.duration);if(t<0)t+=asset.duration;}for(const auto& curve:asset.floatCurves)if(curve.name==n.stateName){v=Anim::SampleCurve(curve,t);break;}}}st.value=ParamValue::FromFloat(v);break;}
    case NodeType::WrapAngle: st.value = ParamValue::FromFloat(PoseOps::WrapAngle(a.Float())); break;
    case NodeType::DeltaAngle: st.value = ParamValue::FromFloat(PoseOps::WrapAngle(b.Float() - a.Float())); break;
    case NodeType::Damp: {
        if (m_dt <= 0 || !st.relevant) { st.value = a; st.velocity = 0; }
        else {
            const float omega = std::max(n.damping, 1e-3f), delta = st.value.Float() - a.Float();
            const float c = st.velocity + omega * delta, decay = std::exp(-omega * m_dt);
            st.value = ParamValue::FromFloat(a.Float() + (delta + c * m_dt) * decay);
            st.velocity = (st.velocity - omega * c * m_dt) * decay;
        }
        st.relevant = true; break;
    }
    case NodeType::Math: {
        const float x = a.Float(), y = b.Float(), z = Value(n.input[2]).Float();
        const float result = n.operation == "add" ? x + y : n.operation == "subtract" ? x-y : n.operation == "multiply" ? x*y
            : n.operation == "divide" ? (std::fabs(y) > 1e-8f ? x/y : 0.f) : n.operation == "min" ? std::min(x,y)
            : n.operation == "max" ? std::max(x,y) : n.operation == "abs" ? std::fabs(x) : n.operation == "clamp" ? std::clamp(x,std::min(y,z),std::max(y,z)) : x+(y-x)*z;
        st.value = ParamValue::FromFloat(result); break;
    }
    case NodeType::Random:
        m_seed ^= m_seed << 13; m_seed ^= m_seed >> 17; m_seed ^= m_seed << 5;
        st.value = ParamValue::FromFloat(static_cast<float>(m_seed >> 8) / 16777216.f); break;
    case NodeType::StateTime: case NodeType::StateWeight: case NodeType::IsInState: case NodeType::IsTransitioning: {
        if (n.reference >= states.size()) { st.value = {}; break; }
        const auto& machine = states[n.reference]; const auto& def = definition->nodes[n.reference];
        if (n.type == NodeType::StateTime) st.value = ParamValue::FromFloat(machine.stateTime);
        else if (n.type == NodeType::StateWeight) st.value = ParamValue::FromFloat(!machine.Transitioning() ? 1.f : std::clamp(machine.elapsed / std::max(machine.duration, 1e-3f), 0.f, 1.f));
        else if (n.type == NodeType::IsTransitioning) st.value = ParamValue::FromInt(machine.Transitioning());
        else st.value = ParamValue::FromInt(machine.active >= 0 && static_cast<size_t>(machine.active) < def.states.size() && def.states[machine.active].name == n.stateName);
        break;
    }
    case NodeType::ClipNormalizedTime: st.value = ParamValue::FromFloat(NormalizedTime(n.reference)); break;
    case NodeType::EventFired: st.value=ParamValue::FromInt(m_previousEvents.contains(n.stateName));break;
    case NodeType::ClipTimeRemaining: {
        float remaining = 0;
        if (n.reference < states.size()) {
            const auto& node = definition->nodes[n.reference]; const auto clip = slots.find(node.slot);
            if (clip != slots.end() && clip->second < m_clips->Count()) {
                const float duration = m_clips->Get(clip->second).duration;
                float time = states[n.reference].time;
                if (node.looping && duration > 0) { time = std::fmod(time, duration); if (time < 0) time += duration; }
                remaining = std::max(0.f, duration - time);
            }
        }
        st.value = ParamValue::FromFloat(remaining); break;
    }
    default: st.value = {}; break;
    }
    return st.value;
}
void Instance::Latch(const std::array<ParamValue,64>& values, uint64_t triggers) {
    params = values; m_triggers |= triggers;
    for (size_t i = 0; i < definition->params.size(); ++i) if (definition->params[i].type == ParamType::Trigger) {
        if (triggers & (1ull << i)) m_triggerTtl[i] = definition->params[i].holdSec;
        params[i] = ParamValue::FromInt((m_triggers & (1ull << i)) != 0);
    }
}
void Instance::Consume(NodeIndex index) {
    if (index >= states.size()) return;
    const auto& n = definition->nodes[index];
    if (n.type == NodeType::Param && n.param >= 0 && definition->params[n.param].type == ParamType::Trigger) m_consumed |= 1ull << n.param;
    if (n.type == NodeType::Select) {
        Consume(n.input[2]);
        Consume(n.input[Value(n.input[2]).Bool() ? 1 : 0]);
        return;
    }
    for (auto child : n.input) if (child != kNoNode) Consume(child);
}
float Instance::NormalizedTime(NodeIndex index) const {
    if (index >= states.size()) return 0;
    const auto& node = definition->nodes[index];
    if (node.type == NodeType::Clip) {
        const auto clip = slots.find(node.slot);
        return clip != slots.end() && clip->second < m_clips->Count() ? states[index].time / std::max(m_clips->Get(clip->second).duration, 1e-3f) : 0;
    }
    if (BlendSpace(index)) return node.cartesian ? states[index].time : states[index].unwrappedPhase;
    if (node.input[0] != kNoNode) return NormalizedTime(node.input[0]);
    return 0;
}
bool Instance::Looping(NodeIndex index) const {
    if (index >= states.size()) return false;
    const auto& node = definition->nodes[index];
    if (node.type == NodeType::Clip) return node.looping;
    if (BlendSpace(index)) return true;
    return node.input[0] != kNoNode && Looping(node.input[0]);
}
Instance::SlotRecord* Instance::FindRequest(SlotHandle handle) {
    if (!handle.id || handle.generation != generation) return nullptr;
    for (auto& request : requests) if (request.handle.id == handle.id && request.handle.generation == handle.generation) return &request;
    return nullptr;
}
bool Instance::Request(SlotHandle handle, const SlotRequestDesc& desc) {
    if (handle.generation != generation) return false;
    bool site = false;
    for (const auto& node : definition->nodes) if (node.type == NodeType::Slot && node.slot == desc.slot) site = true;
    if (!site || !handle.id || desc.blendIn < 0 || desc.blendOut < 0 || !std::isfinite(desc.blendIn) || !std::isfinite(desc.blendOut) || !std::isfinite(desc.rate)) return false;
    int sameSlot = 0;
    for (const auto& request : requests) if (request.desc.slot == desc.slot && (request.status == SlotStatus::Pending || request.status == SlotStatus::Playing || request.status == SlotStatus::BlendingOut)) ++sameSlot;
    if(!pendingQueueEnabled && sameSlot>0 && !desc.interrupt)return false;
    if (sameSlot >= 8) return false;
    SlotRecord* free = nullptr;
    for (auto& request : requests) if (request.handle.id == 0) { free = &request; break; }
    if (!free) for (auto& request : requests) if (request.status >= SlotStatus::Interrupted && (!free || request.handle.id < free->handle.id)) free = &request;
    if (!free) return false;
    *free = SlotRecord{}; free->handle = handle; free->desc = desc; free->status = SlotStatus::Pending;
    return true;
}
bool Instance::Stop(SlotHandle handle, float blendOut) {
    auto* request = FindRequest(handle); if (!request || !std::isfinite(blendOut) || blendOut<0) return false;
    if (request->status == SlotStatus::Pending) request->status = SlotStatus::Interrupted;
    else if (request->status == SlotStatus::Playing) {
        request->outStartWeight = request->weight; request->status = SlotStatus::BlendingOut; request->elapsed = 0; request->desc.blendOut = std::max(0.f, blendOut); request->stopped = true;
    } else return false;
    return true;
}
void Instance::ResetBranch(NodeIndex index) {
    if (index >= states.size()) return;
    auto& st = states[index]; const auto& node = definition->nodes[index];
    st.time = st.previousTime = st.sampleTime = node.startTime; st.relevant = false;
    if (BlendSpace(index)) { BlendSpace(index)->phase = 0; st.unwrappedPhase = 0; }
    for (auto child : node.input) if (child != kNoNode && IsPose(definition->nodes[child].type)) ResetBranch(child);
}
void Instance::SetPhase(NodeIndex index,float phase){
    if(index>=states.size())return;const auto& node=definition->nodes[index];
    if(node.type==NodeType::Clip){auto clip=slots.find(node.slot);if(clip!=slots.end() && clip->second<m_clips->Count())states[index].sampleTime=states[index].time=states[index].previousTime=phase*m_clips->Get(clip->second).duration;}
    if(BlendSpace(index)){BlendSpace(index)->phase=phase-std::floor(phase);states[index].time=states[index].unwrappedPhase=phase;}
    for(auto child:node.input)if(child!=kNoNode && IsPose(definition->nodes[child].type))SetPhase(child,phase);
    for(auto child:node.branches)SetPhase(child,phase);
}
int Instance::Pose(NodeIndex index, float weight, float rate) {
    if (index == kNoNode || index >= states.size()) return AddTask({});
    auto& st = states[index];
    const auto& n = definition->nodes[index];
    st.weight += weight;
    // DAG fan-out emits a single sampling task and retains its buffer.
    if (st.task >= 0) {
        for (auto& tick : ticks) if (tick.node == index) tick.weight += weight;
        return st.task;
    }
    if(n.resetOnRelevant && !st.relevant) ResetBranch(index);
    if(lodEnabled && lod>n.maxLod && (n.type==NodeType::AimOffset || n.type==NodeType::ApplyAdditive || n.type==NodeType::LayeredBlend)){st.task=Pose(n.input[0],weight,rate);return st.task;}
    auto blend = [&](NodeIndex a, NodeIndex b, float alpha, int mask = -1, bool additive = false) {
        alpha = std::clamp(alpha, 0.f, 1.f);
        if (alpha <= kRelevanceEps) return Pose(a, weight, rate);
        if (!additive && mask < 0 && alpha >= 1.f - kRelevanceEps) return Pose(b, weight, rate);
        int ta = Pose(a, additive || mask >= 0 ? weight : weight * (1.f - alpha), rate);
        int tb = Pose(b, weight * alpha, rate);
        return AddTask({ additive ? TaskOp::Additive : TaskOp::Blend, ta, tb, mask, kInvalidClip, 0, alpha });
    };
    switch (n.type) {
    case NodeType::Output: case NodeType::CachedPose: st.task = Pose(n.input[0], weight, rate); break;
    case NodeType::Inertialization: {
        const auto source=Pose(n.input[0],weight,rate);PoseTask task;task.op=TaskOp::Inertialize;task.a=source;
        task.inertializer=&m_inertializers[index];task.dt=m_dt;task.duration=n.blendTime;
        task.inertialRequest=n.input[1]!=kNoNode && Value(n.input[1]).Bool() && inertialEnabled;task.deadBlend=n.deadBlend;
        if(task.inertialRequest)Consume(n.input[1]);st.task=AddTask(task);break;
    }
    case NodeType::Clip: {
        auto it = slots.find(n.slot);
        const auto clip = it == slots.end() ? kInvalidClip : it->second;
        st.task = AddTask({TaskOp::Sample, -1, -1, -1, clip, st.time});
        tasks[st.task].looping=n.looping;
        if (clip < m_clips->Count()) ticks.push_back({index, clip, st.task, weight,
            rate * n.rate * (n.input[0] == kNoNode ? 1.f : Value(n.input[0]).Float()), n.looping});
        break;
    }
    case NodeType::Blend2: st.task = blend(n.input[0], n.input[1], n.input[2] == kNoNode ? n.alpha : Value(n.input[2]).Float()); break;
    case NodeType::BlendByBool: {
        const int target = Value(n.input[2]).Bool() ? 1 : 0;
        if (st.active < 0) st.active = target;
        if (st.active != target) { st.source = st.active; st.active = target; st.elapsed = 0; }
        if (st.source >= 0) {
            if (m_dt > 0) st.elapsed += m_dt;
            else if (st.elapsed == 0) st.elapsed = n.blendTime;
            const float alpha = n.blendTime > 0 ? std::clamp(st.elapsed / n.blendTime, 0.f, 1.f) : 1.f;
            st.task = blend(n.input[st.source], n.input[st.active], alpha);
            if (alpha >= 1) st.source = -1;
        } else st.task = Pose(n.input[st.active], weight, rate);
        break;
    }
    case NodeType::TimeScale: st.task = Pose(n.input[0], weight, rate * (n.input[1] == kNoNode ? n.rate : Value(n.input[1]).Float())); break;
    case NodeType::BlendByInt: {
        if (n.branches.empty()) { st.task = AddTask({}); break; }
        const int target = std::clamp(static_cast<int32_t>(Value(n.input[2]).integer), 0, static_cast<int>(n.branches.size() - 1));
        if (st.active < 0) st.active = target;
        if (st.active != target) { st.source = st.active; st.active = target; st.elapsed = 0; }
        if (st.source >= 0) {
            if (m_dt > 0) st.elapsed += m_dt;
            else if (st.elapsed == 0) st.elapsed = n.blendTime;
            const float alpha = n.blendTime > 0 ? std::clamp(st.elapsed / n.blendTime, 0.f, 1.f) : 1.f;
            st.task = blend(n.branches[st.source], n.branches[st.active], alpha);
            if (alpha >= 1) st.source = -1;
        } else st.task = Pose(n.branches[st.active], weight, rate);
        break;
    }
    case NodeType::ApplyAdditive: st.task = blend(n.input[0], n.input[1], n.input[2] == kNoNode ? n.alpha : Value(n.input[2]).Float(), n.mask, true);if(tasks[st.task].op==TaskOp::Additive)tasks[st.task].meshSpace=n.meshSpace; break;
    case NodeType::LayeredBlend: {
        st.task = Pose(n.input[0], weight, rate);
        for (const auto& layer : n.layers) {
            const float alpha = std::clamp(layer.weight == kNoNode ? layer.alpha : Value(layer.weight).Float(), 0.f, 1.f);
            if (alpha <= kRelevanceEps) continue;
            const int other = Pose(layer.pose, weight * alpha, rate);
            st.task = AddTask({layer.additive ? TaskOp::Additive : TaskOp::Blend, st.task, other, layer.mask, kInvalidClip, 0, alpha});
        }
        break;
    }
    case NodeType::AimOffset: {
        const float alpha=std::clamp(n.input[3]==kNoNode?n.alpha:Value(n.input[3]).Float(),0.f,1.f);
        const int base=Pose(n.input[0],weight,rate);st.task=base;if(alpha<=kRelevanceEps)break;
        const auto weights=CartesianWeights(n,Value(n.input[1]).Float(),Value(n.input[2]).Float());
        float total=0;int aim=-1;
        for(size_t p=0;p<weights.size();++p)if(weights[p]>kRelevanceEps){auto clip=slots.find(n.samples[p].slot);if(clip==slots.end() || clip->second>=m_clips->Count())continue;
            int sample=AddTask({TaskOp::Sample,-1,-1,-1,clip->second,0});aim=aim<0?sample:AddTask({TaskOp::Blend,aim,sample,-1,kInvalidClip,0,weights[p]/(total+weights[p])});total+=weights[p];}
        if(aim>=0){PoseTask task;task.op=TaskOp::Additive;task.a=base;task.b=aim;task.alpha=alpha;task.mask=n.mask;task.reference=AddTask({});task.meshSpace=n.meshSpace;st.task=AddTask(task);}break;
    }
    case NodeType::BlendSpace1D: case NodeType::BlendSpace2D: {
        if(n.cartesian){
            auto weights=CartesianWeights(n,Value(n.input[0]).Float(),Value(n.input[1]).Float());float total=0,period=0;
            for(size_t p=0;p<weights.size();++p){auto clip=slots.find(n.samples[p].slot);if(clip==slots.end() || clip->second>=m_clips->Count())weights[p]=0;else{total+=weights[p];period+=weights[p]*std::max(m_clips->Get(clip->second).duration,1e-3f);}}
            if(total<=kRelevanceEps){st.task=AddTask({});break;}
            period/=total;st.previousTime=st.time;st.time+=m_dt*rate*n.rate/period;float accumulated=0;st.task=-1;
            std::vector<size_t> order(weights.size());for(size_t p=0;p<order.size();++p)order[p]=p;
            std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return weights[a]>weights[b];});
            for(auto p:order)if(weights[p]>kRelevanceEps){uint32_t clip=slots.at(n.samples[p].slot);float duration=m_clips->Get(clip).duration,w=weights[p]/total;
                int sample=AddTask({TaskOp::Sample,-1,-1,-1,clip,(st.time+n.samples[p].phaseOffset)*duration});
                ticks.push_back({index,clip,sample,weight*w,rate*n.rate,true,true,(st.time+n.samples[p].phaseOffset)*duration,(st.previousTime+n.samples[p].phaseOffset)*duration});
                ticks.back().phaseOffset=n.samples[p].phaseOffset;
                st.task=st.task<0?sample:AddTask({TaskOp::Blend,st.task,sample,-1,kInvalidClip,0,w/(accumulated+w)});accumulated+=w;}
            break;
        }
        auto& space = *BlendSpace(index);
        space.speed = Value(n.input[n.type == NodeType::BlendSpace1D ? 0 : 1]).Float();
        space.dirDeg = n.type == NodeType::BlendSpace1D ? 0.f : Value(n.input[0]).Float();
        space.playRate = rate * n.rate;
        for (size_t p = 0; p < n.samples.size(); ++p) {
            const auto it = slots.find(n.samples[p].slot);
            space.samples[p].clip = it == slots.end() ? kInvalidClip : it->second;
        }
        const float oldPhase = space.phase;
        PoseOps::ResolveBlendSpace(&space, *m_clips, m_dt, false);
        st.previousTime = oldPhase; st.time = space.phase;
        float period=0;for(int p=0;p<space.outCount;++p)period+=space.outWeight[p]*std::max(m_clips->Get(space.outClip[p]).duration,1e-3f);
        const float previousPhase=st.unwrappedPhase;st.unwrappedPhase+=m_dt*space.playRate/std::max(period,1e-3f);
        if (space.outCount == 0) { st.task = AddTask({}); break; }
        float accumulated = 0;
        for (int p = 0; p < space.outCount; ++p) {
            const float sampleWeight = space.outWeight[p];
            if (p > 0 && sampleWeight <= .001f) continue; // same skip and order as AnimationSystem's locomotion blend
            const auto clipIndex = space.outClip[p];
            const auto& clip = m_clips->Get(clipIndex);
            const float offset=n.samples[space.outSample[p]].phaseOffset;
            const float sampleTime=offset==0?space.outTime[p]:space.outTime[p]+offset*clip.duration;
            const int sampleTask = AddTask({TaskOp::Sample, -1, -1, -1, clipIndex, sampleTime});
            ticks.push_back({index, clipIndex, sampleTask, weight * sampleWeight, space.playRate, true, true, (st.unwrappedPhase+offset)*clip.duration, (previousPhase+offset)*clip.duration});
            ticks.back().keepSampleTime=true;ticks.back().phaseOffset=offset;
            st.task = p == 0 ? sampleTask : AddTask({TaskOp::Blend, st.task, sampleTask, -1, kInvalidClip, 0, sampleWeight / (accumulated + sampleWeight)});
            accumulated += sampleWeight;
        }
        break;
    }
    case NodeType::StateMachine: {
        bool inertialRequest=false;
        if (n.states.empty()) { st.task = AddTask({}); break; }
        if (st.active < 0) {
            st.active = n.defaultState; st.stateTime = 0;
            events.push_back({index, n.states[st.active].name + ".Enter"});
            events.push_back({index, n.states[st.active].name + ".FullyEntered"});
        }
        st.stateTime += std::max(0.f, m_dt);
        if (st.Transitioning() && m_dt > 0) st.elapsed += m_dt;
        if (st.inertialTransition && st.elapsed >= st.duration) {
            if (st.departing >= 0) events.push_back({index, n.states[st.departing].name + ".FullyExited"});
            events.push_back({index, n.states[st.active].name + ".FullyEntered"});
            st.inertialTransition = false; st.departing = -1;
        }
        const float phase = NormalizedTime(n.states[st.active].pose);
        for (size_t t = 0; t < n.transitions.size(); ++t) {
            const auto& tr = n.transitions[t];
            if (tr.from >= 0 && tr.from != st.active) continue;
            if (tr.to == st.active && !tr.allowSelf) continue;
            if (st.Transitioning() && st.transition >= 0) {
                const auto& current = n.transitions[st.transition];
                if (current.interrupt == 0 || (current.interrupt == 1 && tr.priority >= current.priority)) continue;
            }
            if (tr.exitTime >= 0) {
                // Loop exits below one repeat at each cycle's threshold; larger
                // values name an absolute cycle. One-shots stay eligible at end.
                bool eligible = phase >= tr.exitTime;
                if (Looping(n.states[st.active].pose)) {
                    if (tr.exitTime <= 1.f) {
                        eligible = phase > st.time
                            ? std::floor(phase - tr.exitTime) > std::floor(st.time - tr.exitTime)
                            : phase < st.time && std::ceil(phase - tr.exitTime) < std::ceil(st.time - tr.exitTime);
                    } else eligible = phase >= tr.exitTime && st.time < tr.exitTime;
                }
                if (!eligible) continue;
            }
            if (tr.condition != kNoNode && !Value(tr.condition).Bool()) continue;
            Consume(tr.condition);
            const int previous = st.active;
            const bool interrupted = st.Transitioning();
            if (interrupted) {
                events.push_back({index, "transition_interrupted"});
                const int outgoing = st.source >= 0 ? st.source : st.departing;
                if (outgoing >= 0) events.push_back({index, n.states[outgoing].name + ".FullyExited"});
            }
            st.source = interrupted ? -2 : previous;
            st.active = tr.to; st.duration = tr.duration; st.elapsed = m_dt <= 0 ? tr.duration : m_dt;
            st.transition = static_cast<int>(t); st.stateTime = 0;
            if (tr.resetTarget) ResetBranch(n.states[st.active].pose);
            if(tr.syncPhase)SetPhase(n.states[st.active].pose,phase);
            st.inertialTransition = false; st.departing = previous;
            if (tr.inertialize) {
                inertialRequest = true; st.source = -1;
                st.duration = inertialEnabled && m_dt > 0 ? tr.duration : 0.f;
                st.elapsed = 0; st.inertialTransition = st.duration > 0; st.departing = previous;
                if (!st.inertialTransition) {
                    events.push_back({index, n.states[previous].name + ".FullyExited"});
                    events.push_back({index, n.states[st.active].name + ".FullyEntered"}); st.departing = -1;
                }
            }
            events.push_back({index,"transition",1,n.states[previous].name,n.states[st.active].name,tr.inertialize?"inertialize":"crossfade"});
            events.push_back({index, n.states[previous].name + ".Exit"});
            events.push_back({index, n.states[st.active].name + ".Enter"});
            st.time = NormalizedTime(n.states[st.active].pose);
            break; // one deterministic transition per machine per frame
        }
        if (st.stateTime > 0) st.time = phase;
        int result;
        const float linear = st.source == -1 || st.duration <= 0 ? 1.f : std::clamp(st.elapsed / st.duration, 0.f, 1.f);
        const auto curve = st.transition >= 0 ? n.transitions[st.transition].curve : 0;
        const float alpha = curve == 1 ? linear*linear*(3-2*linear) : curve == 2 ? linear*linear : curve == 3 ? 1-(1-linear)*(1-linear) : linear;
        if (st.source != -1 && alpha < 1.f - kRelevanceEps) {
            int source;
            if (st.source == -2) { PoseTask task; task.op = TaskOp::SnapshotLoad; task.snapshot = &m_snapshots[index]; source = AddTask(task); }
            else source = Pose(n.states[st.source].pose, weight*(1-alpha), rate);
            const int target = alpha > kRelevanceEps ? Pose(n.states[st.active].pose, weight*alpha, rate) : -1;
            if(target>=0){PoseTask task;task.op=TaskOp::Blend;task.a=source;task.b=target;task.alpha=alpha;task.activeA=false;result=AddTask(task);}else result=source;
        } else {
            result = Pose(n.states[st.active].pose, weight, rate);
            if (st.source != -1) {
                const int outgoing = st.source >= 0 ? st.source : st.departing;
                if (outgoing >= 0) events.push_back({index, n.states[outgoing].name + ".FullyExited"});
                events.push_back({index, n.states[st.active].name + ".FullyEntered"}); st.source = -1; st.departing = -1;
            }
        }
        PoseTask save; save.op = TaskOp::Inertialize; save.a = result; save.snapshot = &m_snapshots[index];
        save.inertializer=&m_inertializers[index];save.dt=m_dt;save.duration=st.duration;save.inertialRequest=inertialRequest;save.deadBlend=n.deadBlend;
        st.task = AddTask(save);
        break;
    }
    case NodeType::Slot: {
        SlotRecord* playing = nullptr;
        for (auto& request : requests) if (request.desc.slot == n.slot && (request.status == SlotStatus::Playing || request.status == SlotStatus::BlendingOut)) {
            const auto found = slots.find(request.desc.alias);
            if (found == slots.end() || found->second >= m_clips->Count()) { request.status = SlotStatus::Rejected; continue; }
            const auto& clip = m_clips->Get(found->second);
            if(!request.authoredClock) {
                request.previousTime = request.time;
                request.time += m_dt * request.desc.rate * rate;
                if (!request.desc.looping) request.time = std::clamp(request.time, 0.f, clip.duration);
            }
            request.elapsed += std::max(0.f, m_dt);
            if (request.status == SlotStatus::Playing) {
                request.weight = request.desc.blendIn <= 0 ? 1.f : std::min(1.f, request.elapsed / request.desc.blendIn);
                if (!request.desc.holdLastPose && !request.desc.looping && (request.authoredEnd || request.time >= std::max(0.f, clip.duration - request.desc.blendOut))) {
                    request.outStartWeight = request.weight; request.status = SlotStatus::BlendingOut; request.elapsed = 0;
                } else playing = &request;
            }
            if (request.status == SlotStatus::BlendingOut) {
                request.weight = request.desc.blendOut > 0 ? request.outStartWeight * std::max(0.f, 1.f - request.elapsed / request.desc.blendOut) : 0;
                if (request.weight <= kRelevanceEps) {
                    request.status = SlotStatus::Finished; request.weight = 0; events.push_back({index, "slot_end"});
                }
            }
        }
        SlotRecord* pending = nullptr;
        for (auto& request : requests) if (request.desc.slot == n.slot && request.status == SlotStatus::Pending && (!pending || request.handle.id < pending->handle.id)) pending = &request;
        if (pending) {
            auto clip = slots.find(pending->desc.alias);
            const bool ready = clip != slots.end() && clip->second < m_clips->Count();
            pending->pendingTime += std::max(0.f, m_dt);
            if (!ready && (!pending->desc.queueUntilReady || pending->pendingTime > .25f)) pending->status = SlotStatus::Rejected;
            bool outgoing = false;
            for (const auto& request : requests) if (request.desc.slot == n.slot && request.status == SlotStatus::BlendingOut) outgoing = true;
            if (ready && ((!playing && (!pending->desc.chainAtEnd || !outgoing)) || pending->desc.interrupt)) {
                if (pending->desc.interrupt) for (auto& request : requests) if (&request != pending && request.desc.slot == n.slot && (request.status == SlotStatus::Playing || request.status == SlotStatus::BlendingOut)) {
                    request.status = SlotStatus::Interrupted; request.weight = 0; events.push_back({index, "slot_interrupted"});
                }
                if (auto settings=aliasSettings.find(pending->desc.alias); settings!=aliasSettings.end()) {
                    pending->desc.rate *= settings->second.rate; pending->desc.looping = pending->desc.looping || settings->second.looping;
                }
                pending->status = SlotStatus::Playing; pending->elapsed = m_dt <= 0 ? pending->desc.blendIn : m_dt;
                pending->weight = m_dt <= 0 || pending->desc.blendIn <= 0 ? 1.f : std::min(1.f, pending->elapsed / pending->desc.blendIn);
                events.push_back({index, "slot_begin"});
            }
        }
        st.task = Pose(n.input[0], weight, rate);
        // Request ids order the overlay deterministically across recycled slots.
        std::vector<SlotRecord*> active;
        for (auto& request : requests) if (request.desc.slot == n.slot && (request.status == SlotStatus::Playing || request.status == SlotStatus::BlendingOut) && request.weight > kRelevanceEps) active.push_back(&request);
        std::sort(active.begin(), active.end(), [](const auto* a, const auto* b) { return a->handle.id < b->handle.id; });
        for (auto* request : active) {
            const auto clip = slots.at(request->desc.alias);
            const int sample = AddTask({TaskOp::Sample, -1, -1, -1, clip, request->time});
            tasks[sample].looping=request->desc.looping;
            ticks.push_back({index, clip, sample, weight*request->weight, request->desc.rate, request->desc.looping, true, request->time, request->previousTime, request->handle.id});
            st.task = AddTask({TaskOp::Blend, st.task, sample, n.mask, kInvalidClip, 0, request->weight});
        }
        break;
    }
    default: st.task = AddTask({}); break;
    }
    if(n.eventPolicyAuthored) {
        // Nearest authored policy wins, including through a reused CachedPose.
        std::vector<bool> reachable(tasks.size(),false);
        auto visit=[&](auto&& self,int task)->void {
            if(task<0 || reachable[task])return;reachable[task]=true;
            self(self,tasks[task].a);self(self,tasks[task].b);
        };
        visit(visit,st.task);
        for(auto& tick:ticks)if(tick.eventOwner==kNoNode && reachable[tick.task])tick.eventOwner=index;
    }
    return st.task;
}
bool Instance::SetTimeScale(float scale,float duration) {
    if(!std::isfinite(scale) || !std::isfinite(duration) || scale<0 || duration<0)return false;
    if(duration>0){if(timeScaleRemaining<=0)timeScaleRestore=timeScale;timeScaleRemaining=duration;}
    else{timeScaleRemaining=0;timeScaleRestore=scale;}
    timeScale=scale;return true;
}
bool Instance::Evaluate(const ClipLibrary& clips, float dt, LocalPose* out) {
    m_clips = &clips;
    if(holdRecordedPose && !m_replaying){std::copy(lastOutput.begin(),lastOutput.end(),out);notifyEvents.clear();events.clear();return true;}
    if(m_recording && !m_replaying) {
        if(m_recorded.size()>=m_recordCapacity)m_recording=false;
        else {
            RecordedFrame record;record.dt=dt;record.timeScale=timeScale;record.timeScaleRemaining=timeScaleRemaining;record.timeScaleRestore=timeScaleRestore;record.params=params;record.triggers=m_triggers;record.ttl=m_triggerTtl;
            record.requests=requests;record.slots=slots;record.aliases=aliasSettings;record.world=recordWorld;record.inertialEnabled=inertialEnabled;record.markerSyncEnabled=markerSyncEnabled;record.lodEnabled=lodEnabled;record.rootMotionEnabled=rootMotionEnabled;record.relevanceEndEnabled=relevanceEndEnabled;record.pendingQueueEnabled=pendingQueueEnabled;record.lod=lod;record.updateInterval=updateInterval;
            if(m_recorded.size()%30==0)record.checkpoint=std::make_shared<Snapshot>(Capture());
            m_recorded.push_back(std::move(record));
        }
    }
    m_lodCarry+=dt;
    bool urgent=timeScaleRemaining>0 || m_triggers!=0 || std::memcmp(params.data(),m_previousParams.data(),sizeof(params))!=0;
    for(const auto& request:requests)if(request.status==SlotStatus::Pending || request.status==SlotStatus::Playing || request.status==SlotStatus::BlendingOut)urgent=true;
    const bool skip=lodEnabled && !urgent && dt>0 && updateInterval>1 && m_lodCounter++%uint32_t(updateInterval)!=0;
    if(skip){++skippedFrames;notifyEvents.clear();events.clear();tasks.clear();ticks.clear();rootMotion={};std::copy(lastOutput.begin(),lastOutput.end(),out);if(m_recording && !m_replaying)m_recorded.back().poseHash=poseHash;return true;}
    m_previousParams=params;m_dt=m_lodCarry*timeScale;m_lodCarry=0;
    ++frame;
    tasks.clear(); ticks.clear(); events.clear(); legIKWeight = 0; m_consumed = 0;
    for (auto& st : states) { st.task = -1; st.weight = 0; }
    int root = Pose(definition->root, 1.f);
    if(m_resume || m_rootInertial.Active()) {
        PoseTask resume;resume.op=TaskOp::Inertialize;resume.a=root;resume.inertializer=&m_rootInertial;resume.dt=m_dt;resume.duration=.15f;resume.inertialRequest=m_resume && inertialEnabled;
        root=AddTask(resume);m_resume=false;
    }
    // Recover final relevance from the task DAG. A cached composite used by
    // several parents propagates every contribution to its shared sample leaves.
    std::vector<float> effective(tasks.size(),0),activeWeights(tasks.size(),0);effective[root]=activeWeights[root]=1;
    for(int i=int(tasks.size())-1;i>=0;--i){const auto& t=tasks[i];float w=effective[i],active=activeWeights[i],aWeight=t.op==TaskOp::Blend && t.mask<0?1-t.alpha:1;if(t.a>=0){effective[t.a]+=w*aWeight;if(t.activeA)activeWeights[t.a]+=active*aWeight;}if(t.b>=0){effective[t.b]+=w*t.alpha;activeWeights[t.b]+=active*t.alpha;}}
    // Root motion follows the root bone's actual layered contribution. Upper
    // body masks must not move the motor or attenuate the base root track.
    std::vector<float> rootWeights(tasks.size(),0);rootWeights[root]=1;
    for(int i=int(tasks.size())-1;i>=0;--i){const auto& t=tasks[i];float alpha=t.alpha;
        if(t.mask>=0 && !bound->masks[t.mask].empty())alpha*=bound->masks[t.mask][0];
        if(t.a>=0)rootWeights[t.a]+=rootWeights[i]*(t.op==TaskOp::Blend?1-alpha:1);
        if(t.b>=0)rootWeights[t.b]+=rootWeights[i]*alpha;
    }
    for(auto& tick:ticks){tick.weight=effective[tick.task];tick.activeState=activeWeights[tick.task]>kRelevanceEps;}
    for (auto& tick : ticks) {
        auto& st = states[tick.node];
        const auto& clip = clips.Get(tick.clip);
        if (tick.manual) continue;
        st.previousTime = st.time;
        st.time += m_dt * tick.rate;
        if (clip.duration > 0) {
            if (!tick.looping) st.time = std::clamp(st.time, 0.f, clip.duration);
        }
        tick.time = st.time; tick.previous = st.previousTime;
        // Wrapped sample time, with the same arithmetic as AnimationSystem's clip playback; the unwrapped
        // st.time drives notify crossings and root motion across multiple loops.
        st.sampleTime += m_dt * tick.rate;
        if(clip.duration>0){if(tick.looping){st.sampleTime=std::fmod(st.sampleTime,clip.duration);if(st.sampleTime<0)st.sampleTime+=clip.duration;}else st.sampleTime=std::clamp(st.sampleTime,0.f,clip.duration);}
        else st.sampleTime=0;
        tasks[tick.task].time=st.sampleTime;
        tick.keepSampleTime=true;
    }
    std::unordered_set<std::string> processed;
    for(size_t i=0;i<ticks.size();++i){const auto& group=definition->nodes[ticks[i].node].syncGroup;if(group.empty() || !processed.insert(group).second)continue;
        size_t leader=i;float period=0,total=0;
        for(size_t p=0;p<ticks.size();++p){const auto& candidate=definition->nodes[ticks[p].node];if(candidate.syncGroup!=group)continue;
            if(std::fabs(ticks[p].rate)>1e-6f){period+=ticks[p].weight*std::max(clips.Get(ticks[p].clip).duration,1e-3f)/std::fabs(ticks[p].rate);total+=ticks[p].weight;}
            const auto& incumbent=definition->nodes[ticks[leader].node];bool preferred=candidate.syncRole=="alwaysLeader" && incumbent.syncRole!="alwaysLeader";
            if(candidate.syncRole!="alwaysFollower" && (preferred || incumbent.syncRole=="alwaysFollower" || (candidate.syncRole==incumbent.syncRole && ticks[p].weight>ticks[leader].weight)))leader=p;
        }
        auto& sync=syncStates[group];const auto& leadClip=clips.Get(ticks[leader].clip);
        if(!sync.initialized){sync.phase=ticks[leader].previous/std::max(leadClip.duration,1e-3f)-ticks[leader].phaseOffset;sync.initialized=true;}
        sync.previous=sync.phase;if(total>kRelevanceEps)sync.phase+=m_dt*(ticks[leader].rate<0?-1.f:1.f)/std::max(period/total,1e-3f);
        for(size_t p=0;p<ticks.size();++p)if(definition->nodes[ticks[p].node].syncGroup==group){auto& tick=ticks[p];const auto& clip=clips.Get(tick.clip);tick.leader=p==leader;
            tick.previous=markerSyncEnabled?MarkerTime(leadClip,clip,sync.previous*leadClip.duration):sync.previous*clip.duration;
            tick.time=markerSyncEnabled?MarkerTime(leadClip,clip,sync.phase*leadClip.duration):sync.phase*clip.duration;
            if(tick.phaseOffset!=0){tick.previous+=tick.phaseOffset*clip.duration;tick.time+=tick.phaseOffset*clip.duration;}
            tick.keepSampleTime=false;if(BlendSpace(tick.node)){BlendSpace(tick.node)->phase=sync.phase-std::floor(sync.phase);states[tick.node].time=definition->nodes[tick.node].cartesian?sync.phase:BlendSpace(tick.node)->phase;states[tick.node].unwrappedPhase=sync.phase;}else states[tick.node].time=tick.time;
        }
    }
    rootMotion={};curves.clear();
    for(auto& tick:ticks){const auto& clip=clips.Get(tick.clip);if(!tick.keepSampleTime)tasks[tick.task].time=tick.time;
        if(clip.legDriveMode==LegDriveMode::IK)legIKWeight+=tick.weight;
        if(rootMotionEnabled && definition->nodes[tick.node].rootMotion){auto delta=Anim::RootDelta(clip.rootMotion,tick.previous,tick.time,clip.duration,tick.looping);rootMotion.translation.x+=delta.translation.x*rootWeights[tick.task];rootMotion.translation.y+=delta.translation.y*rootWeights[tick.task];rootMotion.translation.z+=delta.translation.z*rootWeights[tick.task];rootMotion.yaw+=delta.yaw*rootWeights[tick.task];}
        float curveTime=tick.time;if(tick.looping && clip.duration>0){curveTime=std::fmod(curveTime,clip.duration);if(curveTime<0)curveTime+=clip.duration;}
        for(const auto& curve:clip.floatCurves)curves[curve.name]+=Anim::SampleCurve(curve,curveTime)*tick.weight;
    }
    if(auto weight=curves.find("legIKWeight");weight!=curves.end())legIKWeight=weight->second;
    legIKWeight = std::clamp(legIKWeight, 0.f, 1.f);
    m_notifySampler.BeginFrame();
    for(const auto& tick:ticks) {
        const NodeIndex owner=tick.eventOwner==kNoNode?tick.node:tick.eventOwner;
        const auto& node=definition->nodes[owner];
        if(tick.weight <= kRelevanceEps || node.eventPolicy == "none")continue;
        if(node.eventPolicy=="weightAbove" && tick.weight<node.eventThreshold)continue;
        if(node.eventPolicy=="activeState" && !tick.activeState)continue;
        if(node.eventPolicy=="leader" && !definition->nodes[tick.node].syncGroup.empty() && !tick.leader)continue;
        if(node.eventPolicy=="mostActive" || (node.eventPolicy=="leader" && definition->nodes[tick.node].syncGroup.empty())) {
            const TickRecord* most=nullptr;
            for(const auto& candidate:ticks) {
                const auto candidateOwner=candidate.eventOwner==kNoNode?candidate.node:candidate.eventOwner;
                if(candidateOwner==owner && (!most || candidate.weight>most->weight))most=&candidate;
            }
            if(most!=&tick)continue;
        }
        const auto& clip=clips.Get(tick.clip);
        const uint64_t player=(static_cast<uint64_t>(tick.node)<<48) ^ (static_cast<uint64_t>(tick.clip)<<16) ^ tick.player;
        m_notifySampler.Sample(player,tick.node,tick.weight,clip.notifyTracks,tick.previous,tick.time,clip.duration,tick.looping,notifyEvents,definition->nodes[tick.node].id,tick.activeState);
    }
    m_notifySampler.EndFrame(notifyEvents,relevanceEndEnabled);
    m_previousEvents.clear();
    for(const auto& event:events) {m_previousEvents.insert(event.name);debugEvents.push_back(std::to_string(frame)+" "+event.name);}
    for(const auto& event:notifyEvents) {
        const auto& name=event.edge==NotifyEdge::Point?event.point.displayName:event.state.displayName;
        if(event.edge==NotifyEdge::Point)m_previousEvents.insert(name);
        debugEvents.push_back(std::to_string(frame)+" "+name+(event.edge==NotifyEdge::Begin?" Begin":event.edge==NotifyEdge::End?(event.forcedEnd?" ForcedEnd":" End"):" Point"));
    }
    while(debugEvents.size()>64)debugEvents.pop_front();
    for (auto& st : states) st.relevant = st.weight > kRelevanceEps;
    m_triggers &= ~m_consumed;
    for (size_t i = 0; i < definition->params.size(); ++i) if (m_triggers & (1ull << i)) {
        m_triggerTtl[i] -= std::max(0.f, m_dt);
        if (m_triggerTtl[i] <= 0) m_triggers &= ~(1ull << i);
    }
    if(!executor.Execute(tasks, root, *bound, clips, out))return false;
    maxAngularDelta=0;
    for(size_t b=0;b<bound->rest.size();++b) {
        using namespace DirectX;
        const float dot=std::clamp(std::fabs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&lastOutput[b].rot),XMLoadFloat4(&out[b].rot)))),0.f,1.f);
        maxAngularDelta=std::max(maxAngularDelta,2.f*std::acos(dot));
    }
    poseHash=2166136261u;
    const auto* bytes=reinterpret_cast<const uint8_t*>(out);
    for(size_t i=0;i<bound->rest.size()*sizeof(LocalPose);++i)poseHash=(poseHash^bytes[i])*16777619u;
    std::copy_n(out,bound->rest.size(),lastOutput.begin());
    if(m_recording && !m_replaying && !m_recorded.empty()) {
        auto& record=m_recorded.back();record.poseHash=poseHash;record.tasks=tasks;
        // The trace is data: pointers belong to this live instance and are never serialized/replayed.
        for(auto& task:record.tasks){task.snapshot=nullptr;task.inertializer=nullptr;}
    }
    if(timeScaleRemaining>0 && dt>0){timeScaleRemaining=std::max(0.f,timeScaleRemaining-dt);if(timeScaleRemaining<=1e-6f){timeScaleRemaining=0;timeScale=timeScaleRestore;}}
    return true;
}
Instance::Snapshot Instance::Capture() const {
    Snapshot state;state.definitionHash=definition->contentHash;state.generation=generation;state.frame=frame;state.seed=m_seed;
    state.poseHash=poseHash;state.timeScale=timeScale;state.timeScaleRemaining=timeScaleRemaining;state.timeScaleRestore=timeScaleRestore;state.dt=m_dt;state.legIKWeight=legIKWeight;state.maxAngularDelta=maxAngularDelta;
    state.states.assign(states.begin(),states.end());state.params=params;state.slots=slots;state.aliases=aliasSettings;state.requests=requests;
    state.spaces.resize(states.size());for(size_t i=0;i<states.size();++i)if(BlendSpace(i))state.spaces[i]=*BlendSpace(i);
    state.poses=m_snapshots;state.inertializers=m_inertializers;state.rootInertial=m_rootInertial;state.notifies=m_notifySampler;
    state.ttl=m_triggerTtl;state.triggers=m_triggers;state.consumed=m_consumed;state.previousEvents=m_previousEvents;state.debugEvents=debugEvents;
    state.output=lastOutput;state.resume=m_resume;state.syncStates=syncStates;state.rootMotion=rootMotion;state.curves=curves;state.previousParams=m_previousParams;state.carry=m_lodCarry;state.lodCounter=m_lodCounter;state.skippedFrames=skippedFrames;return state;
}
bool Instance::Restore(const Snapshot& state) {
    if(state.definitionHash!=definition->contentHash || state.generation!=generation || state.states.size()!=states.size() || state.output.size()!=bound->rest.size())return false;
    frame=state.frame;m_seed=state.seed;poseHash=state.poseHash;timeScale=state.timeScale;timeScaleRemaining=state.timeScaleRemaining;timeScaleRestore=state.timeScaleRestore;m_dt=state.dt;legIKWeight=state.legIKWeight;maxAngularDelta=state.maxAngularDelta;
    std::copy(state.states.begin(),state.states.end(),states.begin());params=state.params;slots=state.slots;aliasSettings=state.aliases;requests=state.requests;
    for(size_t i=0;i<states.size();++i)if(BlendSpace(i))*BlendSpace(i)=state.spaces[i];
    m_snapshots=state.poses;m_inertializers=state.inertializers;m_rootInertial=state.rootInertial;m_notifySampler=state.notifies;
    m_triggerTtl=state.ttl;m_triggers=state.triggers;m_consumed=state.consumed;m_previousEvents=state.previousEvents;debugEvents=state.debugEvents;
    lastOutput=state.output;m_resume=state.resume;syncStates=state.syncStates;rootMotion=state.rootMotion;curves=state.curves;m_lodCarry=state.carry;m_lodCounter=state.lodCounter;skippedFrames=state.skippedFrames;m_previousParams=state.previousParams;tasks.clear();ticks.clear();notifyEvents.clear();events.clear();return true;
}
bool Instance::Rewind(const ClipLibrary& clips,size_t index) {
    if(index>=m_recorded.size())return false;
    size_t start=index;while(start>0 && !m_recorded[start].checkpoint)--start;
    if(!m_recorded[start].checkpoint || !Restore(*m_recorded[start].checkpoint))return false;
    m_replaying=true;bool ok=true;std::vector<LocalPose> output(bound->rest.size());
    for(size_t i=start;i<=index && ok;++i) {
        const auto& record=m_recorded[i];params=record.params;requests=record.requests;slots=record.slots;aliasSettings=record.aliases;
        m_triggers=record.triggers;m_triggerTtl=record.ttl;timeScale=record.timeScale;timeScaleRemaining=record.timeScaleRemaining;timeScaleRestore=record.timeScaleRestore;inertialEnabled=record.inertialEnabled;markerSyncEnabled=record.markerSyncEnabled;lodEnabled=record.lodEnabled;rootMotionEnabled=record.rootMotionEnabled;relevanceEndEnabled=record.relevanceEndEnabled;pendingQueueEnabled=record.pendingQueueEnabled;lod=record.lod;updateInterval=record.updateInterval;recordWorld=record.world;
        ok=Evaluate(clips,record.dt,output.data()) && poseHash==record.poseHash;
    }
    m_replaying=false;m_recording=false;holdRecordedPose=ok;
    // Rewinding is inspection: gameplay mailboxes must not see historical events.
    notifyEvents.clear();events.clear();return ok;
}

}
