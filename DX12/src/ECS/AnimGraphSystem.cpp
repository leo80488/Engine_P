#include "ECS/AnimGraphSystem.h"
#include "ECS/AnimationSystem.h"
#include "ECS/AnimEntityResolve.h"
#include "Resource/AnimGraphLoader.h"
#include "Resource/AssetFS.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/Check.h"
#include "Gameplay/AttackPlayback.h"
#include "Gameplay/GameplayTime.h"
#include "Anim/Graph/DebugSnapshot.h"
#include <algorithm>
#include <unordered_set>
#include <cstring>

using namespace Anim::Graph;

namespace {
// The command line is fixed after boot, so each switch is looked up once, the first time a tick reaches
// it, not once per entity per tick (CommandLine::Get(key) also allocates a std::string).
struct InstanceSwitches {
    bool off = false, queueOn = true, relevanceEndOn = true, inertialOn = true, markerSyncOn = true, rootMotionOn = true, lodOn = true;
};
const InstanceSwitches& GetInstanceSwitches() {
    static const InstanceSwitches s_switches = [] {
        const auto& cl = Sys::CommandLine::Get();
        InstanceSwitches s;
        s.off = cl.Has("-animgraph_off");
        if (s.off) return s;   // the rest are never asked while the graph is off
        s.queueOn = !cl.Has("-animgraph_queue_off");
        s.relevanceEndOn = !cl.Has("-animgraph_relevance_end_off");
        s.inertialOn = !cl.Has("-animgraph_inertial_off");
        s.markerSyncOn = !cl.Has("-animgraph_marker_off");
        s.rootMotionOn = !cl.Has("-animgraph_root_off");
        s.lodOn = !cl.Has("-animgraph_lod_off");
        return s;
    }();
    return s_switches;
}
bool IkVmdMaskOff() { static const bool s_off = Sys::CommandLine::Get().Has("-ik_vmd_mask_off"); return s_off; }
const std::string& DebugTarget() { static const std::string s_target = Sys::CommandLine::Get().Get("-animgraph_debug"); return s_target; }
const std::string& TraceTarget() { static const std::string s_target = Sys::CommandLine::Get().Get("-animgraph_trace"); return s_target; }
bool AbEnabled() { static const bool s_ab = Sys::CommandLine::Get().Has("-animgraph_ab"); return s_ab; }
}

AnimGraphSystem::AnimGraphSystem(AnimationSystem& animation, SkeletonRegistry& skeletons, ClipLibrary& clips)
    : m_animation(animation), m_skeletons(skeletons), m_clips(clips) { animation.SetPoseSource(this); }
AnimGraphSystem::~AnimGraphSystem() { Reset(); m_animation.SetPoseSource(nullptr); }
void AnimGraphSystem::Release(Entry& e) {
    if (m_clipSystem) for (auto& pair : e.directClips) m_clipSystem->ReleaseClip(pair.second);
    e.directClips.clear();
}
void AnimGraphSystem::CaptureExternalPose(Entity e, const Anim::LocalPose* pose, uint32_t bones) {
    auto found=m_entries.find(e);
    if(found!=m_entries.end() && found->second.suspended && found->second.instance && found->second.instance->bound->rest.size()==bones)
        found->second.instance->ResumeFrom(pose);
}
void AnimGraphSystem::Reset() {
    for (auto& pair : m_entries) Release(pair.second);
    m_entries.clear(); m_bindings.clear(); m_epoch = ~0u;
    m_retired.clear();m_parameterWarnings.clear();m_abDone=false;
}
Instance* AnimGraphSystem::GetInstance(Entity e) const {
    auto it = m_entries.find(e); return it == m_entries.end() ? nullptr : it->second.instance.get();
}
bool AnimGraphSystem::OwnsPose(Entity e) const {
    auto it = m_entries.find(e); return it != m_entries.end() && it->second.owns;
}
void AnimGraphSystem::Prepare(World& world, const GameplayTime::Clocks& clocks) {
    if (world.Epoch() != m_epoch) { Reset(); m_epoch = world.Epoch(); }
    std::vector<Entity> previousOwners;
    for (auto& pair : m_entries) { if(pair.second.owns)previousOwners.push_back(pair.first); pair.second.owns = false; }
    auto* pool = world.GetPool<AnimGraphComponent>();
    if (!m_resources) return;
    world.EnsurePool<AnimGraphParamsComponent>();
    std::unordered_set<Entity> alive;
    for (size_t i = 0; pool && i < pool->Data().size(); ++i) {
        const Entity e = pool->Entities()[i]; auto& component = pool->Data()[i];
        const auto* sk = world.GetComponent<SkeletonComponent>(e);
        auto* animation = world.GetComponent<AnimationComponent>(e);
        if (!sk || !animation || sk->assetIndex >= m_skeletons.Count() || !component.enabled || component.graphPath.empty()) continue;
        alive.insert(e);
        auto& entry = m_entries[e];
        if (entry.generation != world.GetGeneration(e) || entry.path != component.graphPath || entry.skeleton != sk->assetIndex) {
            if(entry.instance && entry.generation==world.GetGeneration(e)) { entry.instance->EndNotifies(); m_retired.push_back({e,entry.generation,std::move(entry.instance->notifyEvents)}); }
            Release(entry); entry = Entry{}; entry.generation = world.GetGeneration(e); entry.path = component.graphPath; entry.skeleton = sk->assetIndex;
        }
        auto& asset = m_assets[component.graphPath];
        if (!asset.handle.IsValid() && !asset.definition) asset.handle = m_resources->Load(component.graphPath, Resource::ResourceType::AnimGraph);
        if (!asset.definition && m_resources->GetState(asset.handle) == Resource::ResourceState::Ready) {
            if (const auto* resource = m_resources->Get<Resource::AnimGraphResource>(asset.handle)) {
                asset.definition = resource->compiled.definition;
                if (!asset.definition && !asset.warned) {
                    for (const auto& d : resource->compiled.diagnostics) LOG_WARNING("[ANIMGRAPH] %s node=%s: %s", component.graphPath.c_str(), d.node.c_str(), d.message.c_str());
                    asset.warned = true;
                }
            }
        }
        if (!asset.definition) continue;
        if (!entry.instance || entry.instance->definition->contentHash != asset.definition->contentHash) {
            auto previous = std::move(entry.instance);
            if(previous) { previous->EndNotifies(); m_retired.push_back({e,entry.generation,std::move(previous->notifyEvents)}); }
            auto& skeleton = m_skeletons.Get(sk->assetIndex);
            if (!skeleton.humanoid.IsValid()) Resource::HumanoidProfileBuilder::Build(skeleton);
            auto& cached=m_bindings[{asset.definition->contentHash,sk->assetIndex}];
            auto binding=cached.lock();if(!binding){binding=BoundGraph::Bind(*asset.definition,skeleton);cached=binding;}
            entry.instance = std::make_unique<Instance>(asset.definition, binding, e ^ world.GetGeneration(e));
            entry.instance->generation = ++m_instanceGeneration;
            auto* parameters = world.GetComponent<AnimGraphParamsComponent>(e);
            std::array<ParamValue, 64> oldValues{};
            if (parameters) oldValues = parameters->values;
            if (!parameters) { world.AddComponent<AnimGraphParamsComponent>(e, {}); parameters = world.GetComponent<AnimGraphParamsComponent>(e); }
            parameters->count = static_cast<uint32_t>(asset.definition->params.size());
            parameters->definitionHash = asset.definition->contentHash;
            parameters->pendingTriggers = 0;
            for (size_t p = 0; p < asset.definition->params.size(); ++p) {
                auto value = asset.definition->params[p].value;
                for (const auto& overrideValue : component.defaults) if (overrideValue.first == asset.definition->params[p].name) value = overrideValue.second;
                if (previous) {
                    const int old = previous->definition->FindParam(asset.definition->params[p].name);
                    if (old >= 0 && previous->definition->params[old].type == asset.definition->params[p].type) value = oldValues[old];
                }
                parameters->values[p] = value;
            }
            component.definitionVersion = asset.definition->contentHash; component.instanceHandle = e;
        }
        const auto* overridePose = world.GetComponent<AnimPoseOverrideComponent>(e);
        if (animation->externalTime || (overridePose && (overridePose->layers & AnimPoseOverrideComponent::Base))) {entry.suspended=true;continue;}
        bool ready = true;
        for (const auto& slot : entry.instance->definition->slots) {
            if (entry.instance->slots.find(slot) == entry.instance->slots.end()) ready = false;
        }
        if (!ready && std::chrono::duration<float>(std::chrono::steady_clock::now() - entry.started).count() < 2.f) continue;
        if (!ready && !entry.warned) { LOG_WARNING("[ANIMGRAPH] e=%u startup timeout; missing clips use reference pose", e); entry.warned = true; }
        const InstanceSwitches& switches=GetInstanceSwitches();
        if (switches.off) continue;
        entry.instance->pendingQueueEnabled=switches.queueOn;
        entry.instance->relevanceEndEnabled=switches.relevanceEndOn;
        entry.instance->inertialEnabled=switches.inertialOn;
        entry.instance->markerSyncEnabled=switches.markerSyncOn;
        entry.instance->rootMotionEnabled=switches.rootMotionOn;
        entry.instance->lodEnabled=switches.lodOn;
        entry.instance->lod=std::max(0,component.lod);entry.instance->updateInterval=std::clamp(component.updateInterval,1,60);
        entry.suspended = false;
        entry.owns = true; entry.previewDt=component.previewDt;
        if(const auto* transform=world.GetComponent<GlobalTransform>(e))
            std::copy_n(&transform->matrix._11,16,entry.instance->recordWorld.begin());
        if (auto* params = world.GetComponent<AnimGraphParamsComponent>(e)) {
            entry.instance->Latch(params->values, params->pendingTriggers);
            params->pendingTriggers = 0;
            for (const auto& request : params->slotRequests) {
                if (request.stop) entry.instance->Stop(request.handle, request.desc.blendOut);
                else entry.instance->Request(request.handle,request.desc);
            }
            params->slotRequests.clear();
        }
        // Attack clocks touch ECS and run in the serial prepare phase, once.
        const float dt=GameplayTime::EntityDt(world.GetComponent<TimeDilationComponent>(e),clocks);
        for(auto& request:entry.instance->requests) {
            request.authoredClock=false;request.authoredEnd=false;
            if(request.status!=Anim::Graph::SlotStatus::Playing && request.status!=Anim::Graph::SlotStatus::BlendingOut)continue;
            const auto clip=entry.instance->slots.find(request.desc.alias);
            if(clip==entry.instance->slots.end() || clip->second>=m_clips.Count())continue;
            request.previousTime=request.time;
            request.authoredClock=Attack::Advance(world,e,request.desc.alias,m_clips.Get(clip->second).duration,
                animation->paused?0.f:(entry.previewDt>=0?entry.previewDt:dt)*entry.instance->timeScale,request.time);
            if(request.authoredClock)if(const auto* playback=world.GetComponent<Attack::Playback>(e))
                request.authoredEnd=playback->progress>=playback->definition->finish-1e-5f;
        }
    }
    for(auto e:previousOwners) { auto it=m_entries.find(e); if(it!=m_entries.end() && !it->second.owns && it->second.instance)it->second.instance->EndNotifies(); }
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (!alive.count(it->first)) {
            if(it->second.instance) { it->second.instance->EndNotifies(); m_retired.push_back({it->first,it->second.generation,std::move(it->second.instance->notifyEvents)}); }
            Release(it->second); it = m_entries.erase(it);
        } else ++it;
    }
}
void AnimGraphSystem::ResolvePendingBinds(World& world) {
    if (!m_clipSystem) return;
    // Render-phase only: binding appends to ClipLibrary after all workers join.
    for (auto& pair : m_entries) {
        auto& entry = pair.second; if (!entry.instance) continue;
        const auto* set = world.GetComponent<AnimationSetComponent>(pair.first);
        const auto* graph=world.GetComponent<AnimGraphComponent>(pair.first);
        if(graph && entry.setPath!=graph->animSetPath) {
            entry.setPath=graph->animSetPath;entry.instance->slots.clear();entry.instance->aliasSettings.clear();
            Release(entry);
        }
        if(graph && !graph->animSetPath.empty()) {
            auto& asset=m_animSets[graph->animSetPath];
            if(!asset.handle.IsValid())asset.handle=m_resources->Load(graph->animSetPath,Resource::ResourceType::AnimSet);
            if(!asset.definition && m_resources->GetState(asset.handle)==Resource::ResourceState::Ready)
                if(const auto* loaded=m_resources->Get<Resource::AnimSetResource>(asset.handle)) {
                    asset.definition=std::make_shared<Resource::AnimSetResource>(*loaded);
                    if(!loaded->error.empty())LOG_WARNING("[ANIMGRAPH] AnimSet %s: %s",graph->animSetPath.c_str(),loaded->error.c_str());
                }
            if(asset.definition)for(const auto& [name,reference]:asset.definition->slots) {
                auto& handle=entry.directClips[reference.path];if(!handle.IsValid())handle=m_clipSystem->AcquireClip(reference.path);
                if(m_clipSystem->IsReady(handle)) {
                    uint32_t clip=kInvalidClip;
                    if(const auto* resource=m_clipSystem->GetResource(handle)) {
                        const uint32_t first=m_clipSystem->BindToSkeleton(handle,m_skeletons.Get(entry.skeleton),m_clips);
                        if(first!=kInvalidClip) {
                            const int source=Resource::FindSourceClip(resource->clips,reference.sourceClip);
                            if(source>=0)clip=first+static_cast<uint32_t>(source);
                        }
                    }
                    entry.instance->slots[name]=clip;
                    entry.instance->aliasSettings[name]={reference.looping,reference.rate};
                }
            }
            continue;
        }
        if (set) for (const auto& ref : set->clips) {
            uint32_t clip = kInvalidClip; bool failed = false;
            if (m_clipSystem->GetBoundAnimatorClip(world, pair.first, ref.name, clip, failed)) { entry.instance->slots[ref.name] = clip; entry.instance->aliasSettings[ref.name]={ref.looping,ref.speed}; }
        }
        for (const auto& slot : entry.instance->definition->slots) {
            uint32_t clip = kInvalidClip; bool failed = false;
            if (set && set->Find(slot)) {
                if (m_clipSystem->GetBoundAnimatorClip(world, pair.first, slot, clip, failed)) entry.instance->slots[slot] = clip;
            } else if (slot.ends_with(".ianim")) {
                auto& handle = entry.directClips[slot];
                if (!handle.IsValid()) handle = m_clipSystem->AcquireClip(slot);
                if (m_clipSystem->IsReady(handle)) {
                    clip = m_clipSystem->GetResource(handle) ? m_clipSystem->BindToSkeleton(handle, m_skeletons.Get(entry.skeleton), m_clips) : kInvalidClip;
                    entry.instance->slots[slot] = clip;
                }
            }
        }
    }
}
bool AnimGraphSystem::EvaluatePose(Entity e, const SkeletonAsset&, Anim::LocalPose* out, uint32_t, float dt) {
    auto* instance = GetInstance(e);if(!instance)return false;
    const auto started=std::chrono::steady_clock::now();const bool result=instance->Evaluate(m_clips,m_entries.at(e).previewDt>=0?m_entries.at(e).previewDt:dt,out);
    instance->evaluationUs=std::chrono::duration<float,std::micro>(std::chrono::steady_clock::now()-started).count();return result;
}
void AnimGraphSystem::ForEachOutput(World& world, const std::function<void(Entity,Instance&)>& callback) {
    for(auto& [e,entry]:m_entries)if(entry.instance && world.IsAlive(e) && entry.generation==world.GetGeneration(e))callback(e,*entry.instance);
}
void AnimGraphSystem::DrainRetired(World& world, const std::function<void(Entity,const SampledNotify&)>& callback) {
    for(auto& retired:m_retired)if(world.IsAlive(retired.entity) && retired.generation==world.GetGeneration(retired.entity))
        for(const auto& event:retired.events)callback(retired.entity,event);
    m_retired.clear();
}
void AnimGraphSystem::Publish(World& world) {
    world.ForEach<AnimGraphComponent>([](Entity,AnimGraphComponent& graph){graph.rootMotion={};});
    for (const auto& pair : m_entries) if (pair.second.owns && pair.second.instance) {
        if (auto* animation = world.GetComponent<AnimationComponent>(pair.first)) {
            animation->legIKWeight=pair.second.instance->legIKWeight;
            animation->activeLegDrive = animation->legIKWeight < .5f ? 1 : 0;
            const auto* skeleton = world.GetComponent<SkeletonComponent>(pair.first);
            if (skeleton && skeleton->assetIndex < m_skeletons.Count()) {
                const auto& asset = m_skeletons.Get(skeleton->assetIndex);
                const bool enabled = animation->legIKWeight > 0.f;
                animation->ikChainMask = enabled ? ~uint64_t(0) : 0;
                animation->ikChainOverflow.assign(asset.ikChains.size() > 64 ? asset.ikChains.size()-64 : 0, enabled);
                const Anim::Graph::TickRecord* dominant = nullptr;
                for (const auto& tick : pair.second.instance->ticks)
                    if (!dominant || tick.weight > dominant->weight) dominant = &tick;
                if (dominant && dominant->clip < m_clips.Count()
                    && !IkVmdMaskOff()) {
                    const auto& clip = m_clips.Get(dominant->clip);
                    float time = dominant->time;
                    if (dominant->looping && clip.duration > 0.f) {
                        time = std::fmod(time, clip.duration);
                        if (time < 0.f) time += clip.duration;
                    }
                    for (const auto& track : clip.ikEnableTracks) {
                        const auto i = track.chainIndex;
                        if (i >= asset.ikChains.size() || track.Sample(time, clip.frameRate)) continue;
                        if (i < 64) animation->ikChainMask &= ~(uint64_t(1) << i);
                        else animation->ikChainOverflow[i-64] = 0;
                    }
                }
            }
        }
        if(auto* graph=world.GetComponent<AnimGraphComponent>(pair.first)){graph->rootMotion=pair.second.instance->rootMotion;graph->rootFrame=pair.second.instance->frame;}
        const std::string& target=DebugTarget();
        if(!target.empty() && (target==std::to_string(pair.first) || target==world.GetName(pair.first)) && pair.second.instance->frame%30==0)
            LOG_INFO("[ANIMGRAPH_DEBUG] e=%u %s",pair.first,DebugSnapshot(*pair.second.instance).dump().c_str());
        for(const auto& event:pair.second.instance->events)LOG_INFO("[ANIMGRAPH] e=%u frame=%u node=%s event=%s",pair.first,
            pair.second.instance->frame,pair.second.instance->definition->nodes[event.source].id.c_str(),event.name.c_str());
        const std::string& trace=TraceTarget();
        if(!trace.empty() && (trace=="*" || trace==std::to_string(pair.first) || trace==world.GetName(pair.first))) {
            for(const auto& event:pair.second.instance->events)if(event.name=="transition")LOG_INFO("[ANIMGRAPH] e=%u sm=%s from=%s to=%s mode=%s frame=%u",pair.first,pair.second.instance->definition->nodes[event.source].id.c_str(),event.from.c_str(),event.to.c_str(),event.mode.c_str(),pair.second.instance->frame);
            if(pair.second.instance->frame%60==0)LOG_INFO("[ANIMGRAPH] summary e=%u frame=%u tasks=%zu samples=%zu pose=%u",pair.first,pair.second.instance->frame,pair.second.instance->tasks.size(),pair.second.instance->ticks.size(),pair.second.instance->poseHash);
        }
        if(AbEnabled() && !m_abDone && !pair.second.instance->ticks.empty()) {
            if(const auto* legacy=world.GetComponent<LocomotionComponent>(pair.first);legacy && legacy->sampleCount>0){m_abDone=true;const auto result=VerifyLegacy(pair.first,180,1,false);LOG_INFO("[ANIMGRAPH_AB] %s",result.dump().c_str());ENGINE_CHECK(result.value("mismatches",-1)==0,"AnimGraph in-process legacy A/B pose mismatch");}
        }
    }
}
ParamHandle AnimGraphSystem::FindParam(World& world,Entity any,std::string_view name)const {
    ParamHandle handle;handle.name=name;
    if(const auto* instance=GetInstance(Anim::ResolveAnimEntity(world,any)))instance->definition->ResolveParam(handle);
    return handle;
}
bool AnimGraphSystem::SetTyped(World& world,Entity any,ParamHandle& handle,ParamType type,ParamValue value) {
    const Entity e=Anim::ResolveAnimEntity(world,any);const auto* instance=GetInstance(e);auto* parameters=world.GetComponent<AnimGraphParamsComponent>(e);
    if(!instance || !parameters || !instance->definition->ResolveParam(handle) || instance->definition->params[handle.index].type!=type || parameters->definitionHash!=instance->definition->contentHash)return false;
    if(type==ParamType::Float || type==ParamType::Vector3)for(float v:value.vector)if(!std::isfinite(v))return false;
    parameters->values[handle.index]=value;return true;
}
bool AnimGraphSystem::Trigger(World& world,Entity any,ParamHandle& handle) {
    const Entity e=Anim::ResolveAnimEntity(world,any);const auto* instance=GetInstance(e);auto* parameters=world.GetComponent<AnimGraphParamsComponent>(e);
    if(!instance || !parameters || !instance->definition->ResolveParam(handle) || instance->definition->params[handle.index].type!=ParamType::Trigger || parameters->definitionHash!=instance->definition->contentHash)return false;
    parameters->pendingTriggers|=1ull<<handle.index;return true;
}
StateInfo AnimGraphSystem::GetState(World& world,Entity any,std::string_view name)const {
    StateInfo result;const auto* instance=GetInstance(Anim::ResolveAnimEntity(world,any));if(!instance)return result;
    for(size_t i=0;i<instance->definition->nodes.size();++i){const auto& node=instance->definition->nodes[i];const auto& state=instance->states[i];
        if(node.type!=NodeType::StateMachine || node.id!=name || state.active<0)continue;
        result.state=result.target=node.states[state.active].name;const int source=state.source>=0?state.source:state.departing;result.source=source>=0?node.states[source].name:"";result.timeInState=state.stateTime;result.transitioning=state.Transitioning();result.progress=result.transitioning?std::clamp(state.elapsed/std::max(state.duration,1e-3f),0.f,1.f):1.f;break;
    }return result;
}
bool AnimGraphSystem::Set(World& world, Entity any, std::string_view name, ParamValue value) {
    const Entity e = Anim::ResolveAnimEntity(world, any); const auto* instance = GetInstance(e);
    auto* params = world.GetComponent<AnimGraphParamsComponent>(e);
    const int index = instance ? instance->definition->FindParam(name) : -1;
    if (!params || index < 0) {const auto key=std::to_string(e)+":"+std::to_string(instance?instance->definition->contentHash:0)+":"+std::string(name);if(m_parameterWarnings.insert(key).second)LOG_WARNING("[ANIMGRAPH] unknown/unbound parameter e=%u name=%.*s",e,static_cast<int>(name.size()),name.data());return false;}
    const auto type=instance->definition->params[index].type;if(type==ParamType::Float || type==ParamType::Vector3)for(float v:value.vector)if(!std::isfinite(v))return false;
    params->values[index] = value; return true;
}
bool AnimGraphSystem::Trigger(World& world, Entity any, std::string_view name) {
    const Entity e = Anim::ResolveAnimEntity(world, any); const auto* instance = GetInstance(e);
    auto* params = world.GetComponent<AnimGraphParamsComponent>(e);
    const int index = instance ? instance->definition->FindParam(name) : -1;
    if (!params || index < 0 || instance->definition->params[index].type != ParamType::Trigger) return false;
    params->pendingTriggers |= 1ull << index; return true;
}
SlotHandle AnimGraphSystem::PlaySlot(World& world, Entity any, const SlotRequestDesc& desc) {
    const Entity e = Anim::ResolveAnimEntity(world, any); const auto* instance = GetInstance(e);
    auto* params = world.GetComponent<AnimGraphParamsComponent>(e);
    if (!params || !instance || params->slotRequests.size() >= 32) return {};
    if(!std::isfinite(desc.rate) || !std::isfinite(desc.blendIn) || !std::isfinite(desc.blendOut) || desc.blendIn<0 || desc.blendOut<0)return {};
    SlotHandle handle{params->nextRequest++, instance->generation};
    if (handle.id == 0) handle.id = params->nextRequest++;
    params->slotRequests.push_back({desc, handle, false}); return handle;
}
bool AnimGraphSystem::StopSlot(World& world, Entity any, SlotHandle handle, float blendOut) {
    const Entity e = Anim::ResolveAnimEntity(world, any); auto* params = world.GetComponent<AnimGraphParamsComponent>(e);
    auto* instance=GetInstance(e);
    if (!params || !instance || !handle.id || handle.generation!=instance->generation || !std::isfinite(blendOut) || blendOut<0 || params->slotRequests.size()>=32) return false;
    SlotRequestDesc desc; desc.blendOut = blendOut; params->slotRequests.push_back({desc, handle, true}); return true;
}
SlotStatus AnimGraphSystem::SlotStatus(World& world, Entity any, SlotHandle handle) const {
    const Entity e = Anim::ResolveAnimEntity(world, any); auto* instance = GetInstance(e);
    if (!instance || instance->generation != handle.generation || !handle.id) return Anim::Graph::SlotStatus::Rejected;
    if (auto* request = instance->FindRequest(handle)) return request->status;
    if (const auto* params = world.GetComponent<AnimGraphParamsComponent>(e))
        for (const auto& request : params->slotRequests) if (request.handle.id == handle.id && request.handle.generation == handle.generation) return Anim::Graph::SlotStatus::Pending;
    return Anim::Graph::SlotStatus::Rejected;
}
float AnimGraphSystem::SlotTime(World& world, Entity any, SlotHandle handle) const {
    auto* instance = GetInstance(Anim::ResolveAnimEntity(world, any));
    const auto* request = instance ? instance->FindRequest(handle) : nullptr; return request ? request->time : 0;
}
bool AnimGraphSystem::Reload(const std::string& path) {
    std::string text;
    if (!Resource::AssetFS::Get().ReadFileText(path, text)) return false;
    if(path.ends_with(".ianimset")) {
        Resource::AnimSetLoader loader;auto loaded=loader.Load(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
        auto* set=static_cast<Resource::AnimSetResource*>(loaded.resource.get());if(!set || !set->error.empty())return false;
        m_animSets[path].definition=std::make_shared<Resource::AnimSetResource>(*set);
        for(auto& [e,entry]:m_entries)if(entry.setPath==path && entry.instance){entry.instance->slots.clear();entry.instance->aliasSettings.clear();Release(entry);}
        return true;
    }
    auto compiled = CompileText(text,[](const std::string& child){std::string text;if(!Resource::AssetFS::Get().ReadFileText(child,text))throw std::runtime_error("SubGraph not found: "+child);return nlohmann::json::parse(text);});
    if (!compiled.Ok()) { for (const auto& d : compiled.diagnostics) LOG_WARNING("[ANIMGRAPH] reload %s node=%s: %s", path.c_str(), d.node.c_str(), d.message.c_str()); return false; }
    m_assets[path].definition = compiled.definition;
    std::vector<std::string> dependents;
    for(const auto& [name,asset]:m_assets)if(name!=path && asset.definition && std::find(asset.definition->dependencies.begin(),asset.definition->dependencies.end(),path)!=asset.definition->dependencies.end())dependents.push_back(name);
    for(const auto& name:dependents){std::string parent;if(Resource::AssetFS::Get().ReadFileText(name,parent)){auto update=CompileText(parent,[](const std::string& child){std::string text;if(!Resource::AssetFS::Get().ReadFileText(child,text))throw std::runtime_error("SubGraph not found: "+child);return nlohmann::json::parse(text);});if(update.Ok())m_assets[name].definition=update.definition;else LOG_WARNING("[ANIMGRAPH] keeping last valid parent: %s",name.c_str());}}
    LOG_INFO("[ANIMGRAPH] reload path=%s", path.c_str()); return true;
}

Instance::SlotRecord* AnimGraphSystem::ActiveSlot(Entity e, std::string_view slot) const {
    auto* instance=GetInstance(e);if(!instance)return nullptr;
    Instance::SlotRecord* result=nullptr;
    for(auto& request:instance->requests)if(request.desc.slot==slot && (request.status==Anim::Graph::SlotStatus::Playing || request.status==Anim::Graph::SlotStatus::BlendingOut))
        if(!result || request.handle.id>result->handle.id)result=&request;
    return result;
}
float AnimGraphSystem::ClipDuration(Entity e, std::string_view alias) const {
    const auto* instance=GetInstance(e);if(!instance)return 0;
    auto clip=instance->slots.find(std::string(alias));return clip!=instance->slots.end() && clip->second<m_clips.Count()?m_clips.Get(clip->second).duration:0;
}

// Runs both real AnimationSystem paths on isolated CPU worlds in this process.
// Assets/bindings are shared with the caller; no main-world clocks are changed.
nlohmann::json AnimGraphSystem::VerifyLegacy(Entity entity,int frames,int actors,bool layers){
    auto entry=m_entries.find(entity);auto* source=GetInstance(entity);
    if(!source || entry==m_entries.end())return {{"error","Graph not ready"}};
    frames=std::clamp(frames,30,600);actors=std::clamp(actors,1,200);
    auto def=std::make_shared<Definition>(*source->definition);const Node* space=nullptr;
    for(const auto& n:def->nodes)if(n.type==NodeType::BlendSpace1D || n.type==NodeType::BlendSpace2D){space=&n;break;}
    if(!space || space->cartesian)return {{"error","Expected a polar locomotion graph"}};
    LocomotionComponent legacySpace;legacySpace.mode=space->type==NodeType::BlendSpace1D?0:1;legacySpace.damping=space->damping;legacySpace.playRate=space->rate;legacySpace.sampleCount=int(space->samples.size());float maxSpeed=0;
    for(size_t p=0;p<space->samples.size();++p){auto clip=source->slots.find(space->samples[p].slot);if(clip==source->slots.end() || clip->second>=m_clips.Count())return {{"error","Clip not resolved"}};legacySpace.samples[p].clip=clip->second;legacySpace.samples[p].speed=space->samples[p].speed;legacySpace.samples[p].angleDeg=space->samples[p].angle;maxSpeed=std::max(maxSpeed,space->samples[p].speed);}
    int speedIndex=def->FindParam("Speed"),dirIndex=def->FindParam("Direction"),weightIndex=-1;
    uint32_t baseClip=legacySpace.samples[0].clip,overlayClip=legacySpace.samples[std::min(1,legacySpace.sampleCount-1)].clip;
    if(layers){
        for(const auto& [name,clip]:source->slots)if(name=="ATTACK_1" && clip<m_clips.Count())overlayClip=clip;
        def=std::make_shared<Definition>();def->params={{"Weight",ParamType::Float,ParamValue::FromFloat(.5f)}};weightIndex=0;
        Node a;a.type=NodeType::Clip;a.slot="Base";Node b=a;b.slot="Overlay";Node w;w.type=NodeType::Param;w.param=0;Node blend;blend.type=NodeType::LayeredBlend;blend.input[0]=0;blend.layers.push_back({1,2,0,1,false});
        def->nodes={a,b,w,blend};def->root=3;def->masks.push_back({"UpperBody","","Spine",1,0});
    }
    auto binding=BoundGraph::Bind(*def,m_skeletons.Get(entry->second.skeleton));
    struct Source final:Anim::IAnimPoseSource{
        const ClipLibrary& clips;std::unordered_map<Entity,std::unique_ptr<Instance>> instances;
        explicit Source(const ClipLibrary& library):clips(library){}
        bool OwnsPose(Entity e)const override{return instances.contains(e);}
        bool EvaluatePose(Entity e,const SkeletonAsset&,Anim::LocalPose* pose,uint32_t,float dt)override{return instances.at(e)->Evaluate(clips,dt,pose);}
        void Reset()override{instances.clear();}
    } graphSource(m_clips);
    auto oldWorld=std::make_unique<World>(),newWorld=std::make_unique<World>();MorphClipLibrary morphs;
    AnimationSystem oldAnimation(m_skeletons,m_clips,morphs),newAnimation(m_skeletons,m_clips,morphs);newAnimation.SetPoseSource(&graphSource);
    std::vector<Entity> oldEntities,newEntities;
    for(int i=0;i<actors;++i){Entity a=oldWorld->CreateEntity(),b=newWorld->CreateEntity();oldEntities.push_back(a);newEntities.push_back(b);SkeletonComponent skeleton;skeleton.assetIndex=entry->second.skeleton;skeleton.boneCount=uint32_t(binding->rest.size());
        oldWorld->AddComponent<SkeletonComponent>(a,skeleton);newWorld->AddComponent<SkeletonComponent>(b,skeleton);AnimationComponent animation;
        if(layers)animation.primaryClip=baseClip;
        oldWorld->AddComponent<AnimationComponent>(a,animation);newWorld->AddComponent<AnimationComponent>(b,{});
        if(layers){AnimLayerComponent component;component.layerCount=1;component.layers[0].clip=overlayClip;component.layers[0].weight=.5f;oldWorld->AddComponent<AnimLayerComponent>(a,std::move(component));}
        else oldWorld->AddComponent<LocomotionComponent>(a,legacySpace);
        auto instance=std::make_unique<Instance>(def,binding,uint32_t(i+1));instance->slots=layers?std::unordered_map<std::string,uint32_t>{{"Base",baseClip},{"Overlay",overlayClip}}:source->slots;if(Sys::CommandLine::Get().Has("-animgraph_wrong_picks")){
            if(layers)std::swap(instance->slots["Base"],instance->slots["Overlay"]);
            else if(space->samples.size()>1)std::swap(instance->slots[space->samples[0].slot],instance->slots[space->samples[1].slot]);
        }
        graphSource.instances[b]=std::move(instance);
    }
    uint64_t mismatches=0;double legacyUs=0,graphUs=0;
    for(int f=0;f<frames+10;++f){float speed=maxSpeed*(.1f+.85f*std::fabs(std::sin(f*.047f))),direction=Anim::PoseOps::WrapAngle(f*13.7f),alpha=.1f+.8f*std::fabs(std::sin(f*.07f));
        for(int i=0;i<actors;++i){auto& graph=*graphSource.instances.at(newEntities[i]);if(layers){graph.params[weightIndex]=ParamValue::FromFloat(alpha);oldWorld->GetComponent<AnimLayerComponent>(oldEntities[i])->layers[0].weight=alpha;}
            else{if(speedIndex>=0)graph.params[speedIndex]=ParamValue::FromFloat(speed);if(dirIndex>=0)graph.params[dirIndex]=ParamValue::FromFloat(direction);auto* lc=oldWorld->GetComponent<LocomotionComponent>(oldEntities[i]);lc->speed=speed;lc->dirDeg=direction;}}
        auto run=[](AnimationSystem& animation,World& world){auto t=std::chrono::steady_clock::now();animation.Update(world,GameplayTime::Clocks{1.f/60,1.f/60});return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count();};
        double a,b;if(f%2){b=run(newAnimation,*newWorld);a=run(oldAnimation,*oldWorld);}else{a=run(oldAnimation,*oldWorld);b=run(newAnimation,*newWorld);}
        if(f>=10){legacyUs+=a;graphUs+=b;}
        for(int i=0;i<actors;++i){const auto* aPose=oldAnimation.GetLocalPose(oldEntities[i]);const auto* bPose=newAnimation.GetLocalPose(newEntities[i]);if(!aPose || !bPose || std::memcmp(aPose,bPose,binding->rest.size()*sizeof(Anim::LocalPose))!=0)++mismatches;}
    }
    LOG_INFO("[ANIMGRAPH_PARITY] mode=%s actors=%d frames=%d mismatches=%llu legacy_us=%.2f graph_us=%.2f ratio=%.3f",layers?"layers":"locomotion",actors,frames,(unsigned long long)mismatches,legacyUs/frames,graphUs/frames,graphUs/std::max(legacyUs,.001));
    return {{"actors",actors},{"frames",frames},{"mismatches",mismatches},{"legacyUs",legacyUs/frames},{"graphUs",graphUs/frames},{"ratio",graphUs/std::max(legacyUs,.001)}};
}
