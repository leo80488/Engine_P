#include "Gameplay/AttackPlayback.h"
#include "ECS/AnimEntityResolve.h"
#include "ECS/AnimationComponents.h"
#include <sol/sol.hpp>
#include "Scripting/LuaEntityId.h"
#include <algorithm>

namespace Attack {
bool Advance(World& world,Entity e,const std::string& alias,float duration,float dt,float& time) {
    auto* p=world.GetComponent<Playback>(e);
    if(!p||!p->active||p->alias!=alias)return false;
    if(!world.IsAlive(p->owner)||world.GetGeneration(p->owner)!=p->ownerGeneration){p->active=false;return false;}
    p->started=true;p->duration=duration;p->previous=p->progress;
    p->elapsed=std::min(p->elapsed+std::max(0.f,dt),p->definition->Duration(duration));
    p->progress=std::min(p->definition->finish,p->definition->ProgressAt(p->elapsed,duration));
    p->windows=0;
    if(dt>0)for(size_t i=0;i<p->definition->windows.size();++i) {
        const auto& w=p->definition->windows[i];
        if(p->previous<w.end&&p->progress>=w.start&&p->progress>p->previous)p->windows|=uint8_t(1u<<i);
    }
    time=p->progress*duration;return true;
}
namespace {
sol::object ToLua(sol::state_view lua,const Json& j) {
    if(j.is_object()||j.is_array()) {
        sol::table t=lua.create_table();
        if(j.is_array()){int i=1;for(const auto& v:j)t[i++]=ToLua(lua,v);}
        else for(auto it=j.begin();it!=j.end();++it)t[it.key()]=ToLua(lua,it.value());
        return sol::make_object(lua,t);
    }
    if(j.is_boolean())return sol::make_object(lua,j.get<bool>());
    if(j.is_number())return sol::make_object(lua,j.get<double>());
    if(j.is_string())return sol::make_object(lua,j.get<std::string>());
    return sol::make_object(lua,sol::lua_nil);
}
}
void RegisterLua(sol::state& lua,World& world) {
    auto t=lua.create_named_table("Attack");
    t.set_function("Load",[](sol::this_state s,const std::string& path) {
        auto d=Load(path);auto j=d->document;j["path"]=path;j["clipSeconds"]=d->clipSeconds;j["distance"]=d->distance;
        j["duration"]=d->Duration(d->clipSeconds);return ToLua(sol::state_view(s),j);
    });
    t.set_function("LoadSet",[](sol::this_state s,const std::string& path){return ToLua(sol::state_view(s),LoadSet(path));});
    t.set_function("Reload",[](const std::string& path){Load(path,true);return true;});
    t.set_function("Sample",[](sol::this_state s,const std::string& path,double seconds) {
        auto d=Load(path);float u=d->ProgressAt(seconds,d->clipSeconds);
        return ToLua(sol::state_view(s),Json{{"progress",u},{"speed",d->baseSpeed*d->speed.Sample(u)},
            {"distance",d->Displacement(u)},{"duration",d->Duration(d->clipSeconds)}});
    });
    t.set_function("Arm",[&world](LuaEntity root,const std::string& path,const std::string& alias,uint32_t instance) {
        Entity owner=LuaId::ResolveLive(world,root);if(owner==NullEntity)return false;
        Entity e=Anim::ResolveAnimEntityOrSelf(world,owner);auto d=Load(path);
        world.EnsurePool<Playback>();if(!world.HasComponent<Playback>(e))world.AddComponent<Playback>(e,{});
        auto& p=*world.GetComponent<Playback>(e);p={};p.owner=owner;p.ownerGeneration=world.GetGeneration(owner);
        p.definition=std::move(d);p.alias=alias;p.instance=instance;p.active=true;return true;
    });
    t.set_function("Cancel",[&world](LuaEntity root) {
        Entity owner=LuaId::ResolveLive(world,root);if(owner==NullEntity)return;
        if(auto* p=world.GetComponent<Playback>(Anim::ResolveAnimEntityOrSelf(world,owner))){p->active=false;p->windows=0;}
    });
    t.set_function("GetState",[&world](sol::this_state s,LuaEntity root)->sol::object {
        sol::state_view l(s);Entity owner=LuaId::ResolveLive(world,root);
        auto* p=owner==NullEntity?nullptr:world.GetComponent<Playback>(Anim::ResolveAnimEntityOrSelf(world,owner));
        if(!p||!p->active)return sol::make_object(l,sol::lua_nil);
        auto t=l.create_table();t["started"]=p->started;t["progress"]=p->progress;t["previous"]=p->previous;
        t["elapsed"]=p->elapsed;t["duration"]=p->definition->Duration(p->duration>0?p->duration:p->definition->clipSeconds);t["instance"]=p->instance;
        t["baseSpeed"]=p->definition->baseSpeed;t["id"]=p->definition->id;
        t["finished"]=p->started&&p->progress>=p->definition->finish-1e-5f;
        t["distance"]=p->definition->Displacement(p->progress);
        t["phase"]=p->progress<p->definition->windup?"windup":p->progress<p->definition->recovery?"strike":"recovery";
        return sol::make_object(l,t);
    });
    t.set_function("MotionSpeed",[&world](LuaEntity root,float dt) {
        Entity owner=LuaId::ResolveLive(world,root);
        auto* p=owner==NullEntity?nullptr:world.GetComponent<Playback>(Anim::ResolveAnimEntityOrSelf(world,owner));
        if(!p||!p->active||!p->started||dt<=0)return 0.f;
        const auto* anim=world.GetComponent<AnimationComponent>(Anim::ResolveAnimEntityOrSelf(world,owner));
        if(anim&&(anim->paused||anim->externalTime))return 0.f;
        const auto& d=*p->definition;
        float next=std::min(d.finish,d.ProgressAt(p->elapsed+dt,p->duration));
        return std::max(0.f,(d.Displacement(next)-d.Displacement(p->progress))/dt);
    });
}
}
