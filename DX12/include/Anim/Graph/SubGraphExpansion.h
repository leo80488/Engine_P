#pragma once
#include "Anim/Graph/AnimGraphCompiler.h"
#include <algorithm>
#include <stdexcept>
namespace Anim::Graph {
// Compile-time composition: no nested runtime instance, and only pin/reference
// fields are renamed. Semantic names and authored string values remain intact.
inline nlohmann::json ExpandSubGraphs(nlohmann::json doc,const GraphResolver& resolver,std::vector<std::string>& stack,std::vector<std::string>& deps) {
    if(doc.value("schema",0)!=1)throw std::runtime_error("Unsupported SubGraph schema");
    auto original=doc.at("nodes");doc["nodes"]=nlohmann::json::array();
    for(auto node:original) {
        if(node.value("type","")!="SubGraph"){doc["nodes"].push_back(std::move(node));continue;}
        const auto path=node.at("path").get<std::string>(), prefix=node.at("id").get<std::string>()+"/";
        if(!resolver)throw std::runtime_error("SubGraph requires an asset resolver: "+path);
        if(stack.size()>=32 || std::find(stack.begin(),stack.end(),path)!=stack.end())throw std::runtime_error("SubGraph dependency cycle/depth: "+path);
        stack.push_back(path);if(std::find(deps.begin(),deps.end(),path)==deps.end())deps.push_back(path);
        auto child=ExpandSubGraphs(resolver(path),resolver,stack,deps);stack.pop_back();
        const auto mapping=node.value("parameters",nlohmann::json::object());
        auto paramName=[&](const std::string& name){return mapping.contains(name)?mapping.at(name).get<std::string>():prefix+name;};
        for(auto param:child.value("parameters",nlohmann::json::array())) {
            auto name=param.at("name").get<std::string>();param["name"]=paramName(name);
            if(mapping.contains(name)) {
                bool found=false;for(const auto& parent:doc.value("parameters",nlohmann::json::array()))if(parent.at("name")==param["name"]){found=true;if(parent.value("type","float")!=param.value("type","float"))throw std::runtime_error("SubGraph parameter mapping type mismatch: "+name);}
                if(!found)throw std::runtime_error("SubGraph mapping to undeclared parameter: "+name);
            }else {if(!doc.contains("parameters"))doc["parameters"]=nlohmann::json::array();doc["parameters"].push_back(param);}
        }
        for(const auto& slot:child.value("slots",nlohmann::json::array())) {
            if(!doc.contains("slots"))doc["slots"]=nlohmann::json::array();
            const auto name=slot.is_string()?slot.get<std::string>():slot.at("name").get<std::string>();
            bool found=false;for(const auto& p:doc["slots"])if((p.is_string()?p.get<std::string>():p.at("name").get<std::string>())==name)found=true;
            if(!found)doc["slots"].push_back(slot);
        }
        for(auto mask:child.value("masks",nlohmann::json::array())){mask["name"]=prefix+mask.at("name").get<std::string>();if(!doc.contains("masks"))doc["masks"]=nlohmann::json::array();doc["masks"].push_back(mask);}
        for(auto group:child.value("syncGroups",nlohmann::json::array())){if(!doc.contains("syncGroups"))doc["syncGroups"]=nlohmann::json::array();doc["syncGroups"].push_back(prefix+group.get<std::string>());}
        auto ref=[&](nlohmann::json& value){if(!value.is_string())throw std::runtime_error("SubGraph pin reference must be a string");value=prefix+value.get<std::string>();};
        for(auto n:child.at("nodes")) {
            ref(n["id"]);
            if(n.contains("in"))for(auto& v:n["in"])ref(v);
            for(const char* key:{"x","y","machine","player"})if(n.contains(key))ref(n[key]);
            if(n.contains("param"))n["param"]=paramName(n["param"].get<std::string>());
            if(n.contains("mask") && n["mask"]!="")n["mask"]=prefix+n["mask"].get<std::string>();
            if(n.contains("syncGroup") && n["syncGroup"]!="")n["syncGroup"]=prefix+n["syncGroup"].get<std::string>();
            if(n.contains("poses"))for(auto& v:n["poses"])ref(v);
            if(n.contains("states"))for(auto& s:n["states"])ref(s["pose"]);
            if(n.contains("transitions"))for(auto& t:n["transitions"])if(t.contains("when"))ref(t["when"]);
            if(n.contains("layers"))for(auto& l:n["layers"]){ref(l["pose"]);if(l.contains("weight") && l["weight"].is_string())ref(l["weight"]);if(l.contains("mask") && l["mask"]!="")l["mask"]=prefix+l["mask"].get<std::string>();}
            doc["nodes"].push_back(std::move(n));
        }
        node["type"]="CachedPose";node["in"]={{"pose",prefix+child.at("root").get<std::string>()}};doc["nodes"].push_back(std::move(node));
    }
    return doc;
}
}
