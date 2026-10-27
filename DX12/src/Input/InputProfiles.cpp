#include "Input/InputProfiles.h"
#include "Input/InputSystem.h"
#include "System/Log.h"
#include "System/PathUtil.h"   // WritableDataDir, U8Path
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace InputProfiles {
namespace {
using Json=nlohmann::json;
// Beside settings.json: the project folder in the editor, the player's own folder in a game (an
// install folder may be read-only). Final before anything defines a profile (main sets the root).
const std::filesystem::path& File() {
    static const std::filesystem::path s_path=Sys::U8Path(Sys::WritableDataDir()+"/input_profiles.json");
    return s_path;
}
bool ReadFile(Json& j) {
    try {
        if (!std::filesystem::exists(File())) { j=Json::object(); return true; }
        std::ifstream in(File()); in>>j;
        return j.is_object() && j.value("version",1)==1;
    } catch (...) { LOG_WARNING("InputProfiles: invalid input_profiles.json; preserving file"); return false; }
}
Json Encode(const Binding& b) {
    return {{"vk1",b.vk1},{"vk2",b.vk2},{"pad",b.pad},{"posX",b.posX},{"negX",b.negX},
        {"posY",b.posY},{"negY",b.negY},{"stick",b.stick},{"trigger",b.trigger},{"mouse",b.mouse}};
}
bool Decode(const Json& j,Binding& b) {
    try {
        if (!j.is_object()) return false;
        Binding n=b;
        n.vk1=j.value("vk1",n.vk1); n.vk2=j.value("vk2",n.vk2); n.pad=j.value("pad",n.pad);
        n.posX=j.value("posX",n.posX); n.negX=j.value("negX",n.negX);
        n.posY=j.value("posY",n.posY); n.negY=j.value("negY",n.negY);
        n.stick=j.value("stick",n.stick); n.trigger=j.value("trigger",n.trigger); n.mouse=j.value("mouse",n.mouse);
        if (!Registry::Valid(n)) return false;
        b=n; return true;
    } catch (...) { return false; }
}
void ApplySaved(Profile& p,const std::string& name,const Json& j) {
    if (!j.contains("profiles") || !j["profiles"].is_object() || !j["profiles"].contains(name)) return;
    const auto& saved=j["profiles"][name];
    if (!saved.is_object()) return;
    for (auto& a:p.actions) if (saved.contains(a.name) && !Decode(saved[a.name],a.binding))
        LOG_WARNING("InputProfiles: ignored invalid binding %s/%s",name.c_str(),a.name.c_str());
}
}
Registry& Registry::Get() { static Registry s_instance; return s_instance; }
bool Registry::Valid(const Binding& b) {
    for (int k:{b.vk1,b.vk2,b.posX,b.negX,b.posY,b.negY}) if(k<0||k>255) return false;
    return b.pad>=0&&b.pad<=65535&&b.stick>=-1&&b.stick<=1&&b.trigger>=-1&&b.trigger<=1;
}
const Profile* Registry::Find(const std::string& name) const {
    auto i=profiles.find(name); return i==profiles.end()?nullptr:&i->second;
}
const Action* Registry::Find(const std::string& name,const std::string& action) const {
    auto p=Find(name); if(!p) return nullptr;
    auto i=p->index.find(action); return i==p->index.end()?nullptr:&p->actions[i->second];
}
bool Registry::Define(const std::string& name,std::vector<Action> actions) {
    if(name.empty()||name.size()>128||actions.empty()) return false;
    Profile p; std::set<std::string> bridges;
    for(auto& a:actions) {
        if(a.name.empty()||a.name.size()>128||!Valid(a.defaults)||p.index.count(a.name)) return false;
        if(!a.bridge.empty()) {
            GameAction legacy;
            if(!bridges.insert(a.bridge).second) return false;
            if(a.bridge=="Move" || a.bridge=="Look") { if(a.kind!=Kind::Axis2D) return false; }
            else if(a.kind!=Kind::Button||!ParseGameAction(a.bridge.c_str(),legacy)||IsHostAction(legacy)
                ||legacy==GameAction::UIConfirm||legacy==GameAction::UICancel) return false;
        }
        a.binding=a.defaults; a.value={}; a.armed=false;
        p.index.emplace(a.name,p.actions.size()); p.actions.push_back(std::move(a));
    }
    Json saved; if(ReadFile(saved)) ApplySaved(p,name,saved);
    // Hot reload keeps unsaved live customizations for surviving actions.
    if(auto old=Find(name)) for(auto& a:p.actions) {
        auto i=old->index.find(a.name);
        if(i!=old->index.end()) a.binding=old->actions[i->second].binding;
    }
    profiles[name]=std::move(p);
    if(active==name) { ResetState(); Input::Get().NeutralizeGameplay(); }
    return true;
}
void Registry::ResetState() {
    for(auto& [_,p]:profiles) for(auto& a:p.actions) { a.value={}; a.armed=false; }
}
bool Registry::Activate(const std::string& name) {
    if(!Find(name)) return false;
    active=name; ResetState(); Input::Get().NeutralizeGameplay(); return true;
}
void Registry::Deactivate() { active.clear(); ResetState(); Input::Get().NeutralizeGameplay(); }
Value Registry::Read(const std::string& profile,const std::string& action) const {
    if(profile!=active) return {};
    auto a=Find(profile,action); return a?a->value:Value{};
}
bool Registry::Set(const std::string& profile,const std::string& action,const Binding& b) {
    if(!Valid(b)) return false;
    auto p=profiles.find(profile); if(p==profiles.end()) return false;
    auto i=p->second.index.find(action); if(i==p->second.index.end()) return false;
    auto& a=p->second.actions[i->second]; a.binding=b; a.value={}; a.armed=false;
    if(profile==active) Input::Get().NeutralizeGameplay();
    return true;
}
bool Registry::ResetBindings(const std::string& name) {
    auto p=profiles.find(name); if(p==profiles.end()) return false;
    for(auto& a:p->second.actions) a.binding=a.defaults;
    ResetState(); Input::Get().NeutralizeGameplay(); return true;
}
void Registry::Evaluate(const Frame& f) {
    auto p=profiles.find(active); if(p==profiles.end()) return;
    auto key=[&](int k) { return f.keys&&k>0&&f.keys[k]?1.f:0.f; };
    for(auto& a:p->second.actions) {
        const bool was=a.value.held; Value v; const auto& b=a.binding;
        if(a.kind==Kind::Button) v.x=(key(b.vk1)||key(b.vk2)||(f.pad.buttons&b.pad))?1.f:0.f;
        else {
            v.x=key(b.posX)-key(b.negX); v.y=a.kind==Kind::Axis2D?key(b.posY)-key(b.negY):0;
            if(v.x==0 && v.y==0) {
                if(b.stick>=0) { v.x=b.stick==0?f.pad.lx:f.pad.rx; v.y=a.kind==Kind::Axis2D?(b.stick==0?f.pad.ly:f.pad.ry):0; }
                if(b.trigger>=0) v.x=b.trigger==0?f.pad.lt:f.pad.rt;
            }
            const float length=std::sqrt(v.x*v.x+v.y*v.y);
            if(length>1) { v.x/=length; v.y/=length; }
        }
        if(b.mouse) { v.mouseX=f.mouseX; v.mouseY=f.mouseY; }
        const bool neutral=std::abs(v.x)<0.0001f&&std::abs(v.y)<0.0001f;
        if(!f.allowed) { a.armed=false; v={}; }
        else if(!a.armed) { a.armed=neutral; v={}; }
        v.held=std::abs(v.x)>0.0001f||std::abs(v.y)>0.0001f;
        v.pressed=v.held&&!was; v.released=!v.held&&was; a.value=v;
    }
}
void Registry::Bridge(ActionState& c) const {
    auto p=Find(active); if(!p) return;
    // A profile owns gameplay, but host transport and UI navigation stay native.
    const auto keep=(1u<<int(GameAction::MenuToggle))|(1u<<int(GameAction::UIConfirm))|(1u<<int(GameAction::UICancel));
    c.held&=keep; c.pressed&=keep; c.released&=keep;
    c.moveX=c.moveY=c.moveMag=0; c.lookMouseDX=c.lookMouseDY=c.lookStickX=c.lookStickY=0;
    c.lookActive=false; c.analogMove=false;
    for(const auto& a:p->actions) {
        const auto& v=a.value;
        if(a.bridge=="Move") { c.moveX=v.x; c.moveY=v.y; c.moveMag=std::sqrt(v.x*v.x+v.y*v.y); c.analogMove=true; }
        else if(a.bridge=="Look") { c.lookStickX=v.x; c.lookStickY=v.y; c.lookMouseDX=v.mouseX; c.lookMouseDY=v.mouseY; c.lookActive=v.held||v.mouseX!=0||v.mouseY!=0; }
        else if(!a.bridge.empty()) {
            GameAction legacy; if(!ParseGameAction(a.bridge.c_str(),legacy)) continue;
            const auto bit=1u<<int(legacy);
            if(v.held)c.held|=bit; if(v.pressed)c.pressed|=bit; if(v.released)c.released|=bit;
        }
    }
}
bool Registry::Save() const {
    Json j; if(!ReadFile(j)) return false;
    try {
        j["version"]=1;
        if(j.contains("profiles")&&!j["profiles"].is_object()) return false;
        for(const auto& [name,p]:profiles) for(const auto& a:p.actions) j["profiles"][name][a.name]=Encode(a.binding);
        const std::filesystem::path path=std::filesystem::absolute(File()),tmp=path.wstring()+L".tmp";
        { std::ofstream out(tmp,std::ios::binary|std::ios::trunc); out<<j.dump(2)<<'\n'; out.flush(); if(!out) return false; }
        if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
        return true;
    } catch (...) { return false; }
}
bool Registry::Reload() {
    Json j; if(!ReadFile(j)) return false;
    for(auto& [name,p]:profiles) { for(auto& a:p.actions) a.binding=a.defaults; ApplySaved(p,name,j); }
    ResetState(); Input::Get().NeutralizeGameplay(); return true;
}
}
