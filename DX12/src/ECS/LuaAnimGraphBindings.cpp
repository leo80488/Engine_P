#include "ECS/LuaAnimGraphBindings.h"
#include "ECS/AnimGraphSystem.h"
#include "ECS/IKRigComponent.h"
#include "ECS/AnimEntityResolve.h"
#include "ECS/AnimationSystem.h"
#include "Anim/Graph/AnimGraphCompiler.h"
#include "Anim/Graph/DebugSnapshot.h"
#include <sol/sol.hpp>
#include <limits>
#include "Scripting/LuaEntityId.h"
#include "Scripting/LuaGc.h"

using namespace Anim::Graph;
namespace {
sol::object ParamObject(sol::state& lua,const ParamValue& value,ParamType type) {
    if(type==ParamType::Float)return sol::make_object(lua,value.Float());
    if(type==ParamType::Bool || type==ParamType::Trigger)return sol::make_object(lua,value.Bool());
    if(type==ParamType::Vector3){auto t=lua.create_table();t["x"]=value.vector[0];t["y"]=value.vector[1];t["z"]=value.vector[2];return sol::make_object(lua,t);}
    return sol::make_object(lua,static_cast<int32_t>(value.integer));
}
}
void RegisterLuaAnimGraphBindings(sol::state& lua, World& world, AnimGraphSystem& system) {
    auto table = lua.create_named_table("AnimGraph");
    table.set_function("DebugSnapshot",[&lua,&world,&system](LuaEntity any)->sol::object {
        const auto* instance=system.GetInstance(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));
        if(!instance)return sol::make_object(lua,sol::nil);
        std::function<sol::object(const nlohmann::json&)> convert=[&](const nlohmann::json& value)->sol::object {
            if(value.is_structured()) {auto t=lua.create_table();if(value.is_array()){int i=1;for(const auto& child:value)t[i++]=convert(child);}else for(auto it=value.begin();it!=value.end();++it)t[it.key()]=convert(it.value());return sol::make_object(lua,t);}
            if(value.is_boolean())return sol::make_object(lua,value.get<bool>());
            if(value.is_number())return sol::make_object(lua,value.get<double>());
            if(value.is_string())return sol::make_object(lua,value.get<std::string>());
            return sol::make_object(lua,sol::nil);
        };return convert(DebugSnapshot(*instance));
    });
    table.set_function("VerifyLegacy",[&lua,&world,&system](LuaEntity any,sol::optional<sol::table> options){int frames=120,actors=1;bool layers=false;if(options){frames=options->get_or("frames",120);actors=options->get_or("actors",1);layers=options->get_or("layers",false);}auto result=system.VerifyLegacy(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)),frames,actors,layers);auto t=lua.create_table();for(auto it=result.begin();it!=result.end();++it){if(it.value().is_number())t[it.key()]=it.value().get<double>();else if(it.value().is_string())t[it.key()]=it.value().get<std::string>();}return t;});
    table.set_function("Bones",[&lua,&world,&system](LuaEntity any){auto list=lua.create_table();auto* instance=system.GetInstance(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));if(instance)for(size_t b=0;b<instance->bound->rest.size();++b){auto bone=lua.create_table();const auto& p=instance->bound->rest[b].pos;bone["name"]=instance->bound->boneNames[b];bone["parent"]=instance->bound->parents[b]+1;bone["length"]=std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);list[int(b+1)]=bone;}return list;});
    table.set_function("IKGoal",[&world](LuaEntity any,sol::table options){
        Entity e=Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any));if(e==NullEntity)return false;
        IKGoalDef goal;goal.rootBone=options.get_or("root",std::string{});goal.midBone=options.get_or("mid",std::string{});
        goal.endBone=options.get_or("end",std::string{});goal.curve=options.get_or("curve",std::string{});
        goal.weight=options.get_or("weight",1.f);goal.curveTarget={options.get_or("x",0.f),options.get_or("y",0.f),options.get_or("z",0.f)};
        goal.polePoint={options.get_or("poleX",0.f),options.get_or("poleY",0.f),options.get_or("poleZ",1.f)};
        if(goal.rootBone.empty() || goal.midBone.empty() || goal.endBone.empty() || !std::isfinite(goal.weight))return false;
        goal.enabled=true;goal.curveDriven=true;goal.refMode=IKRefMode::Virtual;goal.solver=IKSolverKind::TwoBone;
        goal.poleMode=IKPoleMode::WorldPoint;goal.softness=0.f;
        auto* rig=world.GetComponent<IKRigComponent>(e);if(!rig){world.AddComponent<IKRigComponent>(e,{});rig=world.GetComponent<IKRigComponent>(e);}
        for(size_t i=size_t(IKGoalSlot::Custom0);i<kIKGoalCount;++i){rig->goals[i]={};rig->script[i]={};}
        rig->goals[size_t(IKGoalSlot::Custom0)]=std::move(goal);rig->resolveKey=0;rig->maxDistance=0;return true;
    });
    table.set_function("IKWeights",[&lua,&world](LuaEntity any){auto t=lua.create_table();const auto* rig=world.GetComponent<IKRigComponent>(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));if(rig){t["solvedGoals"]=rig->solvedGoals;int i=1;for(auto weight:rig->appliedWeights)t[i++]=weight;}return t;});
    table.set_function("IsInState",[&world,&system](LuaEntity any,const std::string& machine,const std::string& name){const auto* instance=system.GetInstance(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));if(!instance)return false;for(size_t i=0;i<instance->definition->nodes.size();++i){const auto& n=instance->definition->nodes[i];if(n.type==NodeType::StateMachine && n.id==machine && instance->states[i].active>=0)return n.states[instance->states[i].active].name==name;}return false;});
    table.set_function("SetLOD",[&world](LuaEntity any,int lod,int interval){auto* graph=world.GetComponent<AnimGraphComponent>(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));if(!graph || lod<0 || interval<1 || interval>60)return false;graph->lod=lod;graph->updateInterval=interval;return true;});
    table.set_function("MotorRootMotion",[&world](LuaEntity any,bool enabled){auto* graph=world.GetComponent<AnimGraphComponent>(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));if(!graph)return false;graph->motorRootMotion=enabled;return true;});
    table.set_function("SetTimeScale",[&world,&system](LuaEntity any,float scale,sol::optional<float> duration) {
        auto* instance=system.GetInstance(Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any)));
        return instance && instance->SetTimeScale(scale,duration.value_or(0));
    });
    table["TimeScale"]=table["SetTimeScale"];
    lua.new_usertype<SlotHandle>("AnimGraphSlotHandle", sol::no_constructor, "id", &SlotHandle::id, "generation", &SlotHandle::generation);
    LuaGc::DropTrivialFinalizer<SlotHandle>(lua.lua_state());   // no __gc (Scripting/LuaGc.h)
    table.set_function("PlaySlot", [&world, &system](LuaEntity any, const std::string& slot, const std::string& alias, sol::optional<sol::table> options) {
        SlotRequestDesc desc; desc.slot = slot; desc.alias = alias;
        if (options) { auto t = *options; desc.blendIn = t.get_or("blendIn", .1f); desc.blendOut = t.get_or("blendOut", .2f); desc.rate = t.get_or("rate", 1.f); desc.interrupt = t.get_or("interrupt", false); desc.queueUntilReady = t.get_or("queueUntilReady", false); desc.chainAtEnd = t.get_or("chainAtEnd", false); desc.holdLastPose=t.get_or("holdLastPose",false); desc.looping=t.get_or("loop",false); }
        return system.PlaySlot(world, LuaId::ResolveLive(world, any), desc);
    });
    table.set_function("SlotStatus", [&world, &system](LuaEntity any, SlotHandle handle) {
        static const char* kNames[] = {"pending", "playing", "blendingOut", "interrupted", "finished", "rejected"};
        return std::string(kNames[static_cast<int>(system.SlotStatus(world, LuaId::ResolveLive(world, any), handle))]);
    });
    table.set_function("SlotTime", [&world, &system](LuaEntity any, SlotHandle handle) { return system.SlotTime(world, LuaId::ResolveLive(world, any), handle); });
    table.set_function("StopSlot", [&world, &system](LuaEntity any, SlotHandle handle, sol::optional<float> blend) { return system.StopSlot(world, LuaId::ResolveLive(world, any), handle, blend.value_or(.2f)); });
    table.set_function("GetState", [&lua, &world, &system](LuaEntity any, const std::string& name) {
        auto result = lua.create_table(); const auto* instance = system.GetInstance(Anim::ResolveAnimEntity(world, LuaId::ResolveLive(world, any)));
        if (instance) for (size_t i = 0; i < instance->definition->nodes.size(); ++i) {
            const auto& node = instance->definition->nodes[i]; const auto& state = instance->states[i];
            if (node.type != NodeType::StateMachine || node.id != name) continue;
            result["state"] = state.active >= 0 ? node.states[state.active].name : "";
            result["target"]=state.active>=0?node.states[state.active].name:"";const int source=state.source>=0?state.source:state.departing;result["source"]=source>=0?node.states[source].name:"";
            result["time"] = state.stateTime; result["transitioning"] = state.Transitioning();
            result["progress"] = !state.Transitioning() ? 1.f : std::clamp(state.elapsed / std::max(state.duration, 1e-3f), 0.f, 1.f);
        }
        return result;
    });
    table.set_function("Assign", [&world](LuaEntity any, const std::string& path) {
        const Entity e = Anim::ResolveAnimEntity(world, LuaId::ResolveLive(world, any));
        if (e == NullEntity || !world.HasComponent<SkeletonComponent>(e)) return false;
        AnimGraphComponent component; component.graphPath = path;
        world.AddComponent<AnimGraphComponent>(e, std::move(component)); return true;
    });
    table.set_function("Set", [&world, &system](LuaEntity any, const std::string& name, sol::object value) {
        const Entity e = Anim::ResolveAnimEntity(world, LuaId::ResolveLive(world, any));
        const auto* inst = system.GetInstance(e); const int index = inst ? inst->definition->FindParam(name) : -1;
        if (index < 0) {system.Set(world,e,name,{});return false;}
        const auto type = inst->definition->params[index].type;
        ParamValue param;
        if (type == ParamType::Bool || type == ParamType::Trigger) { if (!value.is<bool>()) return false; param = ParamValue::FromInt(value.as<bool>()); }
        else if (type == ParamType::Float) { if (!value.is<double>()) return false; param = ParamValue::FromFloat(value.as<float>()); }
        else if (type == ParamType::Id && value.is<std::string>()) param = ParamValue::FromInt(static_cast<int32_t>(Hash(value.as<std::string>())));
        else if (type == ParamType::Vector3) {
            if (!value.is<sol::table>()) return false;
            auto t = value.as<sol::table>();
            const char* fields[] = {"x", "y", "z"};
            for (int axis=0; axis<3; ++axis) {
                sol::object component=t[fields[axis]];
                if (!component.is<double>()) return false;
                param.vector[axis]=component.as<float>();
            }
        } else {
            if (!value.is<double>()) return false;
            const double number=value.as<double>();
            const double maximum=type==ParamType::Id?double(std::numeric_limits<uint32_t>::max()):double(std::numeric_limits<int32_t>::max());
            if (!std::isfinite(number) || std::trunc(number)!=number || number<std::numeric_limits<int32_t>::min() || number>maximum) return false;
            param=ParamValue::FromInt(type==ParamType::Id?static_cast<int32_t>(static_cast<uint32_t>(number)):static_cast<int32_t>(number));
        }
        if(type==ParamType::Float || type==ParamType::Vector3)for(float v:param.vector)if(!std::isfinite(v))return false;
        return system.Set(world, e, name, param);
    });
    table.set_function("SetTrigger", [&world, &system](LuaEntity any, const std::string& name) { return system.Trigger(world, LuaId::ResolveLive(world, any), name); });
    table.set_function("Get", [&lua, &world, &system](LuaEntity any, const std::string& name) -> sol::object {
        const Entity e = Anim::ResolveAnimEntity(world, LuaId::ResolveLive(world, any));
        const auto* inst = system.GetInstance(e); const auto* params = world.GetComponent<AnimGraphParamsComponent>(e);
        const int index = inst ? inst->definition->FindParam(name) : -1;
        if (!params || index < 0) return sol::make_object(lua, sol::nil);
        const auto& value = params->values[index]; const auto type = inst->definition->params[index].type;
        return ParamObject(lua,value,type);
    });
    table.set_function("GetParams",[&lua,&world,&system](LuaEntity any){auto t=lua.create_table();const Entity e=Anim::ResolveAnimEntity(world,LuaId::ResolveLive(world,any));const auto* instance=system.GetInstance(e);const auto* params=world.GetComponent<AnimGraphParamsComponent>(e);if(instance && params)for(size_t i=0;i<instance->definition->params.size();++i){const auto& p=instance->definition->params[i];t[p.name]=ParamObject(lua,params->values[i],p.type);}return t;});
    table.set_function("Describe", [&lua, &world, &system](LuaEntity any) {
        const Entity e = Anim::ResolveAnimEntity(world, LuaId::ResolveLive(world, any));
        auto t = lua.create_table(); const auto* inst = system.GetInstance(e);
        t["entity"] = LuaId::Out(world, e); t["ownsPose"] = system.OwnsPose(e); t["ready"] = inst != nullptr;
        if (inst) { t["contentHash"] = std::to_string(inst->definition->contentHash); t["tasks"] = inst->tasks.size(); t["frame"] = inst->frame; t["legIKWeight"] = inst->legIKWeight;
            auto parameters = lua.create_table();
            for(size_t i=0;i<inst->definition->params.size();++i){const auto& p=inst->definition->params[i];const auto& v=inst->params[i];if(p.type==ParamType::Float)parameters[p.name]=v.Float();else if(p.type==ParamType::Bool || p.type==ParamType::Trigger)parameters[p.name]=v.Bool();else if(p.type==ParamType::Vector3){auto vector=lua.create_table();vector["x"]=v.vector[0];vector["y"]=v.vector[1];vector["z"]=v.vector[2];parameters[p.name]=vector;}else parameters[p.name]=int32_t(v.integer);}
            t["parameters"] = parameters;
            t["samples"] = inst->ticks.size();
            if(const auto* component=world.GetComponent<AnimGraphComponent>(e))t["path"]=component->graphPath;
            t["events"]=inst->events.size()+inst->notifyEvents.size();auto states=lua.create_table();
            for(size_t i=0;i<inst->definition->nodes.size();++i)if(inst->definition->nodes[i].type==NodeType::StateMachine){auto state=lua.create_table();const auto& runtime=inst->states[i];const auto& node=inst->definition->nodes[i];state["state"]=runtime.active>=0?node.states[runtime.active].name:"";state["transitioning"]=runtime.Transitioning();state["time"]=runtime.stateTime;states[node.id]=state;}t["states"]=states;
        }
        return t;
    });
}
