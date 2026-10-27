#include "Input/InputProfiles.h"
#include "Input/InputSystem.h"
#include "Scripting/LuaDoc.h"
#include "System/CommandLine.h"
#include <sol/sol.hpp>
#include <array>

namespace {
using namespace InputProfiles;
Binding ReadBinding(sol::table t,Binding b={}) {
    b.vk1=t.get_or("vk1",b.vk1); b.vk2=t.get_or("vk2",b.vk2); b.pad=t.get_or("pad",b.pad);
    b.posX=t.get_or("posX",b.posX); b.negX=t.get_or("negX",b.negX);
    b.posY=t.get_or("posY",b.posY); b.negY=t.get_or("negY",b.negY);
    b.stick=t.get_or("stick",b.stick); b.trigger=t.get_or("trigger",b.trigger); b.mouse=t.get_or("mouse",b.mouse);
    return b;
}
sol::table BindingTable(sol::state_view lua,const Binding& b) {
    auto t=lua.create_table();
    t["vk1"]=b.vk1; t["vk2"]=b.vk2; t["pad"]=b.pad;
    t["posX"]=b.posX; t["negX"]=b.negX; t["posY"]=b.posY; t["negY"]=b.negY;
    t["stick"]=b.stick; t["trigger"]=b.trigger; t["mouse"]=b.mouse;
    return t;
}
}
void RegisterInputProfiles(sol::state& lua) {
    using namespace InputProfiles;
    auto t=lua.create_table();
    t.set_function("Define",[](const std::string& name,sol::table definitions) {
        std::vector<Action> actions;
        for(size_t i=1;i<=definitions.size();++i) {
            sol::table d=definitions[i]; Action a;
            a.name=d.get_or("name",std::string{}); a.bridge=d.get_or("bridge",std::string{});
            const auto kind=d.get_or("type",std::string("button"));
            if(kind=="button") a.kind=Kind::Button;
            else if(kind=="axis1d") a.kind=Kind::Axis1D;
            else if(kind=="axis2d") a.kind=Kind::Axis2D;
            else return false;
            sol::optional<sol::table> b=d["binding"]; if(b) a.defaults=ReadBinding(*b);
            actions.push_back(std::move(a));
        }
        return Registry::Get().Define(name,std::move(actions));
    });
    t.set_function("Exists",[](const std::string& p) {return Registry::Get().Find(p)!=nullptr;});
    t.set_function("Activate",[](const std::string& p) {return Registry::Get().Activate(p);});
    t.set_function("Deactivate",[] {Registry::Get().Deactivate();});
    t.set_function("Active",[] {return Registry::Get().Active();});
    t.set_function("Flush",[] {Registry::Get().ResetState(); Input::Get().NeutralizeGameplay();});
    t.set_function("GetBinding",[](sol::this_state s,const std::string& p,const std::string& name)->sol::object {
        sol::state_view l(s); auto a=Registry::Get().Find(p,name);
        return a?sol::make_object(l,BindingTable(l,a->binding)):sol::make_object(l,sol::nil);
    });
    t.set_function("SetBinding",[](const std::string& p,const std::string& name,sol::table changes) {
        auto a=Registry::Get().Find(p,name); if(!a) return false;
        return Registry::Get().Set(p,name,ReadBinding(changes,a->binding));
    });
    t.set_function("ResetBindings",[](const std::string& p) {return Registry::Get().ResetBindings(p);});
    t.set_function("Save",[] {return Registry::Get().Save();});
    t.set_function("Reload",[] {return Registry::Get().Reload();});
    t.set_function("List",[](sol::this_state s,const std::string& name) {
        sol::state_view l(s); auto out=l.create_table(); auto p=Registry::Get().Find(name); int i=1;
        if(p) for(const auto& a:p->actions) {
            auto d=l.create_table(); d["name"]=a.name; d["bridge"]=a.bridge;
            d["type"]=a.kind==Kind::Button?"button":a.kind==Kind::Axis1D?"axis1d":"axis2d";
            out[i++]=d;
        }
        return out;
    });
    t.set_function("Value",[](sol::this_state s,const std::string& p,const std::string& a) {
        sol::state_view l(s); auto v=Registry::Get().Read(p,a); auto r=l.create_table();
        r["x"]=v.x; r["y"]=v.y; r["mouseX"]=v.mouseX; r["mouseY"]=v.mouseY;
        r["held"]=v.held; r["pressed"]=v.pressed; r["released"]=v.released; return r;
    });
    t.set_function("Held",[](const std::string& p,const std::string& a){return Registry::Get().Read(p,a).held;});
    t.set_function("Pressed",[](const std::string& p,const std::string& a){return Registry::Get().Read(p,a).pressed;});
    t.set_function("Released",[](const std::string& p,const std::string& a){return Registry::Get().Read(p,a).released;});
    t.set_function("Axis",[](const std::string& p,const std::string& a){auto v=Registry::Get().Read(p,a); return std::make_tuple(v.x,v.y);});
    t.set_function("GlyphToken",[](sol::this_state s,const std::string& p,const std::string& name)->sol::object {
        sol::state_view l(s); auto a=Registry::Get().Find(p,name); if(!a) return sol::make_object(l,sol::nil);
        char path[48]; const auto& b=a->binding;
        const bool pad=Input::Get().LastDevice()==InputDeviceKind::Gamepad;
        if(pad&&b.pad&&FormatBindingPath(true,static_cast<uint16_t>(b.pad),path,sizeof(path))) return sol::make_object(l,std::string(path));
        const int key=b.vk1?b.vk1:b.vk2;
        if(key&&FormatBindingPath(false,static_cast<uint16_t>(key),path,sizeof(path))) return sol::make_object(l,std::string(path));
        return sol::make_object(l,sol::nil);
    });
    // Explicit harness-only seam: exercises the SAME native resolver with
    // deterministic cooked device frames. Never creates OS input events.
    const auto& cl=Sys::CommandLine::Get();
    bool seam=cl.Has("-unattended")&&cl.Has("-exec");
#if defined(WITH_HARNESS)
    // -dump_catalog binds it too, so the catalog (and the LuaLS definitions made from it)
    // know the -exec seam. Not in Retail: the flag's name is one of tools/strip_gate.py's
    // markers.
    seam=seam||(cl.Has("-unattended")&&cl.Has("-dump_catalog"));
#endif
    if(seam) {
        LuaDoc::Fn(t,"InputProfile","_TestFrame",[](sol::table source) {
            std::array<uint8_t,256> keys{}; Frame f; f.keys=keys.data();
            sol::optional<sol::table> list=source["keys"];
            if(list) for(auto& item:*list) {int k=item.second.as<int>(); if(k>0&&k<256) keys[k]=1;}
            f.pad.buttons=static_cast<uint16_t>(source.get_or("pad",0));
            f.pad.lx=source.get_or("lx",0.f); f.pad.ly=source.get_or("ly",0.f);
            f.pad.rx=source.get_or("rx",0.f); f.pad.ry=source.get_or("ry",0.f);
            f.pad.lt=source.get_or("lt",0.f); f.pad.rt=source.get_or("rt",0.f);
            f.mouseX=source.get_or("mouseX",0.f); f.mouseY=source.get_or("mouseY",0.f);
            f.allowed=source.get_or("allowed",true); Registry::Get().Evaluate(f);
        },
            { .doc="Feed the input resolver one cooked device frame (no OS input events): held virtual keys, pad buttons and sticks, mouse deltas.", .params={ { .name="frame", .doc="{keys = {vk, ...}, pad = buttons, lx, ly, rx, ry, lt, rt, mouseX, mouseY, allowed = true}" } }, .effect=LuaDoc::Effect::World, .context=LuaDoc::Context::Harness });
    }
    lua["InputProfile"]=t;
}
