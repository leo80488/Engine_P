#include "ECS/LuaIKBindings.h"
#include "ECS/IKRigComponent.h"
#include "Scripting/LuaMathTypes.h"
#include <sol/sol.hpp>
#include "Scripting/LuaEntityId.h"
#include "System/Log.h"
#include <algorithm>
namespace {
int Slot(const std::string& name) {
    for(size_t i=0;i<kIKGoalCount;++i)if(name==kIKGoalNames[i])return int(i);
    return -1;
}
IKRigComponent* Rig(World& world,LuaEntity id) {
    const auto e=LuaId::ResolveLive(world,id);
    return e?world.GetComponent<IKRigComponent>(e):nullptr;
}
bool Valid(const LuaVec3& v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
bool Valid(const LuaQuat& q) {return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w)
    && std::isfinite(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);}
}
void RegisterLuaIKBindings(sol::state& lua,World& world) {
    auto ik=lua.create_named_table("IK");
    auto configure=[](IKScriptTarget& target,const sol::table& options) {
        sol::optional<LuaVec3> offset=options["offset"];
        if(offset && !Valid(*offset))return false;
        target.position=offset?DirectX::XMFLOAT3{offset->x,offset->y,offset->z}:DirectX::XMFLOAT3{0,0,0};
        sol::optional<LuaQuat> rot=options["rotOffset"];
        if(rot && !Valid(*rot))return false;
        auto q=rot?rot->Normalized():LuaQuat{};
        target.rotation={q.x,q.y,q.z,q.w};target.hasRotation=true;
        const float weight=options.get_or("weight",1.f),blend=options.get_or("blend",.2f);
        if(!std::isfinite(weight)||!std::isfinite(blend))return false;
        target.weight=std::clamp(weight,0.f,1.f);
        target.blend=std::max(0.f,blend);target.active=true;return true;
    };
    ik.set_function("SetGoalTarget",[&world,configure](LuaEntity id,const std::string& name,LuaEntity targetId,sol::table options) {
        auto* rig=Rig(world,id);int i=Slot(name);const auto target=LuaId::ResolveLive(world,targetId);
        if(!rig || i<0 || !target)return false;
        IKScriptTarget request;if(!configure(request,options))return false;
        request.space=IKTargetSpace::Entity;request.socket=options.get_or("socket",std::string{});
        request.entity.BindAndStamp(world,target);rig->script[i]=std::move(request);return true;
    });
    ik.set_function("SetGoalBone",[&world,configure](LuaEntity id,const std::string& name,const std::string& bone,sol::table options) {
        auto* rig=Rig(world,id);int i=Slot(name);if(!rig || i<0 || bone.empty())return false;
        IKScriptTarget request;if(!configure(request,options))return false;
        request.space=IKTargetSpace::SelfBone;request.socket=bone;rig->script[i]=std::move(request);return true;
    });
    ik.set_function("SetTwoHandGrip",[&world,configure](LuaEntity id,sol::table options) {
        auto* rig=Rig(world,id);if(!rig)return false;
        IKScriptTarget request;if(!configure(request,options))return false;
        request.space=IKTargetSpace::SelfBone;request.socket=options.get_or("socket",std::string("@RightHand"));
        rig->script[2]=std::move(request);rig->goals[2].handRot=IKHandRot::MatchTarget;return true;
    });
    ik.set_function("SetGrounding",[&world](LuaEntity id,bool enabled,sol::optional<float> blend) {
        auto* rig=Rig(world,id);if(!rig || !std::isfinite(blend.value_or(.1f)))return false;
        rig->ground.enabled=enabled;rig->groundingBlend=std::max(0.f,blend.value_or(.1f));return true;
    });
    ik.set_function("SetFootLock",[&world](LuaEntity id,bool enabled) {
        auto* rig=Rig(world,id);if(!rig)return false;rig->ground.footLock=enabled;return true;
    });
    ik.set_function("GetGroundInfo",[&world,&lua](LuaEntity id) {
        auto result=lua.create_table();auto* rig=Rig(world,id);if(!rig)return result;
        result["pelvisOffset"]=rig->pelvisOffset;result["grounded"]=rig->grounded;
        const auto& left=rig->rt[0].normal;const auto& right=rig->rt[1].normal;
        result["leftNormal"]=LuaVec3{left.x,left.y,left.z};result["rightNormal"]=LuaVec3{right.x,right.y,right.z};
        return result;
    });
    ik.set_function("SetGoal",[&world](LuaEntity id,const std::string& name,sol::table options) {
        auto* rig=Rig(world,id);int i=Slot(name);if(!rig || i<0)return false;
        sol::optional<LuaVec3> requested=options["pos"];
        if(!requested || !Valid(*requested))return false;
        const auto pos=*requested;
        const auto space=options.get_or("space",std::string("world"));
        if(space!="world" && space!="entity")return false;
        const float weight=options.get_or("weight",1.f),blend=options.get_or("blend",.2f);
        sol::optional<LuaQuat> rot=options["rot"];
        if(!std::isfinite(weight)||!std::isfinite(blend)||(rot&&!Valid(*rot)))return false;
        auto& target=rig->script[i];target.space=space=="world"?IKTargetSpace::World:IKTargetSpace::EntityLocal;
        target.position={pos.x,pos.y,pos.z};target.active=true;
        target.weight=std::clamp(weight,0.f,1.f);
        target.blend=std::max(0.f,blend);
        target.hasRotation=bool(rot);
        if(rot) {auto q=rot->Normalized();target.rotation={q.x,q.y,q.z,q.w};}
        return true;
    });
    ik.set_function("ClearGoal",[&world](LuaEntity id,const std::string& name,sol::optional<float> blend) {
        auto* rig=Rig(world,id);int i=Slot(name);if(!rig || i<0 || !std::isfinite(blend.value_or(.2f)))return false;
        rig->script[i].active=false;rig->script[i].blend=std::max(0.f,blend.value_or(.2f));return true;
    });
    ik.set_function("SetWeight",[&world](LuaEntity id,const std::string& name,float weight,sol::optional<float> blend) {
        auto* rig=Rig(world,id);int i=Slot(name);if(!rig || i<0 || !std::isfinite(weight) || !std::isfinite(blend.value_or(.2f)))return false;
        rig->script[i].weight=std::clamp(weight,0.f,1.f);rig->script[i].blend=std::max(0.f,blend.value_or(.2f));return true;
    });
    ik.set_function("GetGoalState",[&world,&lua](LuaEntity id,const std::string& name) {
        auto result=lua.create_table();auto* rig=Rig(world,id);int i=Slot(name);if(!rig || i<0)return result;
        const auto& rt=rig->rt[i];result["path"]=IKPathName(rt.path);result["reached"]=rt.reached;
        result["error"]=rt.error;result["weight"]=rt.weight;result["scriptWeight"]=rt.scriptWeight;result["locked"]=rt.locked;
        result["groundDelta"]=rt.groundDelta;result["effector"]=LuaVec3{rt.effector.x,rt.effector.y,rt.effector.z};
        result["hitEntity"]=LuaId::Out(world,rt.hitEntity);
        return result;
    });
}
