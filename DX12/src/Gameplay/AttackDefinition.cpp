#include "Gameplay/AttackDefinition.h"
#include "Gameplay/DataSchema.h"
#include "Resource/AssetFS.h"
#include "Resource/AnimationLoader.h"
#include "Resource/AnimationResource.h"
#include "System/PathUtil.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <windows.h>

namespace Attack {
namespace {
std::mutex mutex;
std::unordered_map<std::string,std::shared_ptr<const Definition>> cache;
std::unordered_map<std::string,float> durations;
// The range check an attack's fields are read with is the data tables' too (Gameplay/DataSchema.h).
using Gameplay::Data::Number;
Curve ReadCurve(const Json& j,bool motion) {
    if (!j.is_array()||j.size()<2||j.size()>128) throw std::runtime_error("Curve requires 2..128 keys");
    Curve c; float last=-1,previous=-1;
    for (const auto& k:j) {
        Key p; p.u=Number(k,"u",-1,0,1); p.value=Number(k,"value",-1,motion?0.f:.05f,motion?1.f:4.f);
        p.in=Number(k,"in",0,-100,100); p.out=Number(k,"out",0,-100,100);
        p.interp=k.value("interp",std::string("linear"));
        if(p.u<=last||(motion&&p.value<previous)) throw std::runtime_error("Curve keys must increase; motion cannot reverse");
        if(p.interp!="linear"&&p.interp!="step"&&p.interp!="hermite") throw std::runtime_error("Unknown curve interpolation");
        if(motion&&p.interp!="linear") throw std::runtime_error("Motion uses continuous piecewise linear distance keys");
        last=p.u; previous=p.value; c.keys.push_back(p);
    }
    if(c.keys.front().u!=0||c.keys.back().u!=1) throw std::runtime_error("Curve endpoints must be 0 and 1");
    if(motion&&(c.keys.front().value!=0||c.keys.back().value!=1)) throw std::runtime_error("Motion must span distance 0..1");
    return c;
}
}
float Curve::Sample(float u) const {
    if(keys.empty()) return 1;
    auto it=std::upper_bound(keys.begin(),keys.end(),u,[](float x,const Key& k){return x<k.u;});
    if(it==keys.begin())return it->value;
    if(it==keys.end())return keys.back().value;
    const auto& b=*it; const auto& a=*(it-1); float t=(u-a.u)/(b.u-a.u);
    if(a.interp=="step")return a.value;
    if(a.interp=="hermite") {
        float t2=t*t,t3=t2*t,h=b.u-a.u;
        float v=(2*t3-3*t2+1)*a.value+(t3-2*t2+t)*h*a.out+(-2*t3+3*t2)*b.value+(t3-t2)*h*b.in;
        return std::clamp(v,std::min(a.value,b.value),std::max(a.value,b.value));
    }
    return a.value+(b.value-a.value)*t;
}
double Definition::TimeAt(float u) const {
    double p=std::clamp(double(u),0.,1.)*Samples;
    int i=std::min(int(p),Samples-1);
    return clock[i]+(clock[i+1]-clock[i])*(p-i);
}
float Definition::ProgressAt(double elapsed,float duration) const {
    if(duration<=0)return 0;
    double t=std::clamp(elapsed/duration,0.,clock.back());
    auto it=std::upper_bound(clock.begin(),clock.end(),t);
    if(it==clock.end())return 1;
    int i=std::max(0,int(it-clock.begin())-1);
    return float((i+(t-clock[i])/(clock[i+1]-clock[i]))/Samples);
}
float Definition::Displacement(float u) const {return distance*motion.Sample(std::clamp(u/moveEnd,0.f,1.f));}
float ClipDuration(const std::string& path) {
    {std::lock_guard lock(mutex);if(auto it=durations.find(path);it!=durations.end())return it->second;}
    std::vector<uint8_t> bytes;
    if(!Resource::AssetFS::Get().ReadFile(path,bytes))throw std::runtime_error("Missing animation: "+path);
    auto result=Resource::AnimationLoader{}.Load(path,bytes);
    auto* anim=dynamic_cast<Resource::AnimationResource*>(result.resource.get());
    if(!anim||anim->clips.empty()||anim->clips[0].duration<=0)throw std::runtime_error("Invalid bone animation: "+path);
    float d=anim->clips[0].duration;
    std::lock_guard lock(mutex); durations[path]=d;return d;
}
std::shared_ptr<const Definition> Parse(const Json& j,const std::string& path) {
    if(j.value("schemaVersion",0)!=1)throw std::runtime_error("Unsupported attack schemaVersion");
    auto d=std::make_shared<Definition>();d->document=j;d->path=path;
    d->id=j.at("id").get<std::string>(); d->clip=j.at("clip").get<std::string>();d->alias=j.at("alias").get<std::string>();
    if(d->id.empty()||d->alias.empty()||!d->clip.ends_with(".ianim"))throw std::runtime_error("Attack requires id, alias and .ianim clip");
    d->clipSeconds=ClipDuration(d->clip);
    d->speed=ReadCurve(j.at("speedCurve"),false);d->motion=ReadCurve(j.at("motionCurve"),true);
    d->baseSpeed=Number(j,"baseSpeed",1,.05f,4);
    d->windup=Number(j,"windup",.24f,0,1);d->recovery=Number(j,"recovery",.52f,d->windup,1);
    d->finish=Number(j,"finish",.91f,d->recovery,1);
    d->cost=Number(j,"cost",18,0,10000);d->damage=Number(j,"damage",36,0,100000);
    d->staminaDamage=Number(j,"staminaDamage",32,0,100000);d->blendIn=Number(j,"blendIn",.07f,0,2);
    d->blendOut=Number(j,"blendOut",.2f,0,2);
    d->moveEnd=Number(j,"moveEnd",.42f,.001f,d->finish);
    // Without "distance", "legacyMoveSpeed" (default 0.55) x moveEnd x clipSeconds gives it: the
    // distance the source clip moved.
    d->distance=Number(j,"distance",j.value("legacyMoveSpeed",.55f)*d->moveEnd*d->clipSeconds,0,30);
    d->turnEnd=Number(j,"turnEnd",0,0,d->finish);d->turnRate=Number(j,"turnRate",6,0,30);
    d->comboStart=Number(j,"comboStart",.64f,0,d->finish);d->comboEnd=Number(j,"comboEnd",d->finish,d->comboStart,d->finish);
    d->next=j.value("next",std::string{});
    if(!j.contains("cancel")||!j["cancel"].is_array())throw std::runtime_error("cancel must be an array");
    for(const auto& action:j["cancel"])
        if(!action.is_string()||action.get<std::string>()!="dodge")throw std::runtime_error("Supported attack cancel: dodge");
    const auto& hit=j.at("hitbox");
    const auto source=hit.value("source",std::string("weapon"));
    if(source!="weapon"&&source!="owner")throw std::runtime_error("hitbox.source must be weapon or owner");
    d->weaponCollider=source=="weapon";
    d->weaponTag=hit.value("weaponTag",std::string{});
    for(const char* key:{"requireTags","ignoreTags"}) {
        if(!hit.contains(key))continue;
        if(!hit[key].is_array())throw std::runtime_error(std::string("hitbox.")+key+" must be an array of tag names");
        auto& list=key[0]=='r'?d->requireTags:d->ignoreTags;
        for(const auto& name:hit[key]) {
            std::string why;
            const Gameplay::Tag tag=name.is_string()?Gameplay::TagTable::Get().Intern(name.get<std::string>(),&why):Gameplay::Tag{};
            if(!tag.IsValid())throw std::runtime_error(std::string("hitbox.")+key+": "+(why.empty()?"a tag name must be a string":why));
            list.push_back(tag);
        }
    }
    d->radius=Number(hit,"radius",.85f,.01f,10);d->halfHeight=Number(hit,"halfHeight",.3f,0,10);
    d->offsetX=Number(hit,"x",0,-20,20);d->offsetY=Number(hit,"y",.95f,-20,20);d->offsetZ=Number(hit,"z",.85f,-20,20);
    const auto& windows=j.at("hitWindows");
    if(!windows.is_array()||windows.empty()||windows.size()>4)throw std::runtime_error("Attack requires 1..4 hit windows");
    float previous=-1;
    for(const auto& w:windows) {
        Window v{Number(w,"start",.24f,0,d->finish),Number(w,"end",.52f,0,d->finish)};
        if(v.end<=v.start||v.start<previous)throw std::runtime_error("Hit windows must be ordered, nonoverlapping ranges");
        previous=v.end;d->windows.push_back(v);
    }
    d->clock[0]=0;
    // Midpoint quadrature excludes step discontinuities at right endpoints.
    for(int i=1;i<=Definition::Samples;++i)
        d->clock[i]=d->clock[i-1]+1.0/(Definition::Samples*d->baseSpeed*d->speed.Sample((i-.5f)/Definition::Samples));
    return d;
}
std::shared_ptr<const Definition> Load(const std::string& path,bool reload) {
    if(!reload){std::lock_guard lock(mutex);if(auto it=cache.find(path);it!=cache.end())return it->second;}
    std::string text;if(!Resource::AssetFS::Get().ReadFileText(path,text))throw std::runtime_error("Missing attack: "+path);
    auto d=Parse(Json::parse(text),path);
    std::lock_guard lock(mutex);cache[path]=d;return d;
}
Json LoadSet(const std::string& path,bool reload) {
    std::string text;if(!Resource::AssetFS::Get().ReadFileText(path,text))throw std::runtime_error("Missing attack set: "+path);
    auto j=Json::parse(text);
    if(j.value("schemaVersion",0)!=1||!j.at("attacks").is_object()||j["attacks"].empty())throw std::runtime_error("Invalid or empty attack set");
    std::unordered_map<std::string,bool> ids;
    for(const auto& [key,value]:j.at("attacks").items()) {
        auto d=Load(value.get<std::string>(),reload);
        if(!ids.emplace(d->id,true).second)throw std::runtime_error("Duplicate attack ID in set: "+d->id);
        if(!d->next.empty()&&!j["attacks"].contains(d->next))throw std::runtime_error("Missing combo target: "+d->next);
    }
    return j;
}
void Apply(const std::string& path,const Json& j) {auto d=Parse(j,path);std::lock_guard lock(mutex);cache[path]=d;}
void Save(const std::string& path,const Json& j) {
    Parse(j,path);
    const auto dst=Sys::U8Path(path);const auto tmp=Sys::U8Path(path+".tmp");
    if(!dst.parent_path().empty())std::filesystem::create_directories(dst.parent_path());
    {std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("Cannot write attack temp file");f<<j.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("Attack write failed");}
    if(!MoveFileExW(tmp.c_str(),dst.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Attack replace failed; draft retained");
}
}
