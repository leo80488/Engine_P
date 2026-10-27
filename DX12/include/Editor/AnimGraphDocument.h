#pragma once
#include "Anim/Graph/AnimGraphCompiler.h"
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace Editor {
// The canvas and harness apply the same atomic operations and undo snapshots.
class AnimGraphDocument {
public:
    void BeginGesture(){m_gesture=true;m_gestureCommitted=false;}
    void EndGesture(){m_gesture=false;m_gestureCommitted=false;}
    AnimGraphDocument() { New(); }
    void New() { m_graph={{"schema",1},{"name","Animation Graph"},{"parameters",nlohmann::json::array()},
        {"slots",nlohmann::json::array()},{"masks",nlohmann::json::array()},{"root","Output"},
        {"nodes",{{{"id","Reference"},{"type","RefPose"}},{{"id","Output"},{"type","Output"},{"in",{{"pose","Reference"}}}}}}};
        m_saved=m_graph;m_undo.clear();m_redo.clear();++m_revision; }
    bool Load(std::string_view text,std::string& why) {
        try { auto next=nlohmann::json::parse(text);if(!next.contains("nodes") || next.value("schema",0)!=1){why="Expected graph schema 1";return false;}
            m_graph=std::move(next);m_saved=m_graph;m_undo.clear();m_redo.clear();++m_revision;return true;
        }catch(const std::exception& e){why=e.what();return false;}
    }
    const nlohmann::json& Graph()const{return m_graph;}
    uint64_t Revision()const{return m_revision;}
    bool Dirty()const{return m_graph!=m_saved;}
    void Saved(){m_saved=m_graph;}
    std::string Text()const{return m_graph.dump(2)+"\n";}
    bool Undo(){if(m_undo.empty())return false;m_redo.push_back(m_graph);m_graph=std::move(m_undo.back());m_undo.pop_back();++m_revision;return true;}
    bool Redo(){if(m_redo.empty())return false;m_undo.push_back(m_graph);m_graph=std::move(m_redo.back());m_redo.pop_back();++m_revision;return true;}
    const Anim::Graph::CompileResult& Compile(){if(m_revision!=m_compiledRevision){m_compile=Anim::Graph::Compile(m_graph,resolver);m_compiledRevision=m_revision;}return m_compile;}
    nlohmann::json Copy(const std::vector<std::string>& ids)const {
        auto fragment=m_graph;fragment["nodes"]=nlohmann::json::array();fragment.erase("root");
        for(const auto& node:m_graph["nodes"])if(std::find(ids.begin(),ids.end(),node.at("id").get<std::string>())!=ids.end())fragment["nodes"].push_back(node);
        return fragment;
    }
    bool Paste(const nlohmann::json& fragment,std::string& why) {
        try {
            auto next=m_graph;std::map<std::string,std::string> ids;std::set<std::string> used;
            for(const auto& node:next["nodes"])used.insert(node.at("id"));
            for(const auto& node:fragment.at("nodes")){
                const std::string old=node.at("id");int suffix=1;std::string fresh;
                do{fresh=old+"_copy"+std::to_string(suffix++);}while(used.contains(fresh));
                if(ids.contains(old))throw std::runtime_error("Duplicate clipboard node");ids[old]=fresh;used.insert(fresh);
            }
            for(const char* field:{"parameters","slots","masks","syncGroups"})if(fragment.contains(field))for(const auto& value:fragment[field]){
                if(!next.contains(field))next[field]=nlohmann::json::array();bool exists=false;
                for(const auto& current:next[field])if(current==value || (current.is_object() && value.is_object() && current.value("name","")==value.value("name",""))){
                    if(current!=value)throw std::runtime_error(std::string("Clipboard declaration conflicts: ")+field);exists=true;break;
                }
                if(!exists)next[field].push_back(value);
            }
            auto remap=[&](nlohmann::json& reference){if(reference.is_string()){const auto name=reference.get<std::string>();reference=ids.contains(name)?ids.at(name):std::string();}};
            for(auto node:fragment.at("nodes")){
                const std::string old=node.at("id");node["id"]=ids.at(old);
                if(node.contains("in"))for(auto it=node["in"].begin();it!=node["in"].end();){remap(it.value());if(it.value()=="")it=node["in"].erase(it);else ++it;}
                for(const char* key:{"x","y","player","machine"})if(node.contains(key))remap(node[key]);
                if(node.contains("poses"))for(auto& pose:node["poses"])remap(pose);
                if(node.contains("layers"))for(auto& layer:node["layers"]){remap(layer["pose"]);if(layer.contains("weight") && layer["weight"].is_string())remap(layer["weight"]);}
                if(node.contains("states"))for(auto& state:node["states"])remap(state["pose"]);
                if(node.contains("transitions"))for(auto& tr:node["transitions"])if(tr.contains("when"))remap(tr["when"]);
                next["nodes"].push_back(std::move(node));
                auto positions=fragment.value("editor",nlohmann::json::object()).value("positions",nlohmann::json::object());
                if(positions.contains(old)){auto xy=positions[old].get<std::array<float,2>>();next["editor"]["positions"][ids.at(old)]={xy[0]+32,xy[1]+32};}
            }
            Commit(std::move(next));return true;
        }catch(const std::exception& e){why=e.what();return false;}
    }
    bool Apply(const nlohmann::json& operations,std::string& why) {
        try {
            auto next=m_graph;bool validateLinks=false;
            const auto& ops=operations.is_array()?operations:operations.at("ops");
            for(const auto& op:ops) {
                const auto verb=op.at("op").get<std::string>(),id=op.value("node","");
                auto find=[&](const std::string& key)->nlohmann::json& {
                    for(auto& node:next["nodes"])if(node.at("id")==key)return node;
                    throw std::runtime_error("Unknown node: "+key);
                };
                if(verb=="add") {
                    auto node=op.value("props",nlohmann::json::object());node["id"]=op.at("id");node["type"]=op.at("type");
                    for(const auto& existing:next["nodes"])if(existing.at("id")==node["id"])throw std::runtime_error("Duplicate node id");
                    next["nodes"].push_back(node);
                    if(op.contains("pos"))next["editor"]["positions"][op.at("id").get<std::string>()]=op.at("pos");
                } else if(verb=="remove") {
                    if(next.at("root")==id)throw std::runtime_error("Cannot delete root output");
                    find(id);
                    auto& nodes=next["nodes"];nodes.erase(std::remove_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.at("id")==id;}),nodes.end());
                    for(auto& node:nodes){
                        if(node.contains("in"))for(auto it=node["in"].begin();it!=node["in"].end();)if(it.value()==id)it=node["in"].erase(it);else ++it;
                        for(const char* key:{"x","y","player","machine"})if(node.contains(key) && node[key]==id)node.erase(key);
                        if(node.contains("poses")){auto& poses=node["poses"];poses.erase(std::remove(poses.begin(),poses.end(),nlohmann::json(id)),poses.end());}
                        if(node.contains("layers")){auto& layers=node["layers"];layers.erase(std::remove_if(layers.begin(),layers.end(),[&](auto& l){return l.at("pose")==id;}),layers.end());for(auto& l:layers)if(l.contains("weight") && l["weight"]==id)l.erase("weight");}
                        if(node.contains("states"))for(auto& state:node["states"])if(state.at("pose")==id)state["pose"]="";
                        if(node.contains("transitions"))for(auto& transition:node["transitions"])if(transition.contains("when") && transition["when"]==id)transition.erase("when");
                    }
                    next["editor"]["positions"].erase(id);
                } else if(verb=="connect") {
                    const auto from=op.at("from").get<std::string>(),to=op.at("to").get<std::string>();find(from);
                    find(to)["in"][op.at("pin").get<std::string>()]=from;validateLinks=true;
                } else if(verb=="disconnect") find(op.at("to"))["in"].erase(op.at("pin").get<std::string>());
                else if(verb=="set") {auto& node=find(id);for(auto it=op.at("props").begin();it!=op.at("props").end();++it)node[it.key()]=it.value();}
                else if(verb=="move") {find(id);next["editor"]["positions"][id]=op.at("pos");}
                else if(verb=="state_move") {find(id);next["editor"]["statePositions"][id][op.at("state").get<std::string>()]=op.at("pos");}
                else if(verb=="parameter" || verb=="slot" || verb=="mask") {
                    const char* field=verb=="parameter"?"parameters":verb=="slot"?"slots":"masks";
                    next[field].push_back(op.at("value"));
                } else if(verb=="replace") next=op.at("graph");
                else throw std::runtime_error("Unknown graph operation: "+verb);
            }
            if(validateLinks)for(const auto& error:Anim::Graph::Compile(next,resolver).diagnostics)
                if(!error.warning && (error.message.find("Cycle")!=std::string::npos || error.message.find("type mismatch")!=std::string::npos)) {why=error.message;return false;}
            Commit(std::move(next));return true;
        }catch(const std::exception& e){why=e.what();return false;}
    }

private:
    void Commit(nlohmann::json next) {
        if(next==m_graph)return;
        if(!m_gesture || !m_gestureCommitted) {m_undo.push_back(m_graph);if(m_undo.size()>64)m_undo.pop_front();}
        if(m_gesture)m_gestureCommitted=true;
        m_redo.clear();m_graph=std::move(next);++m_revision;
    }

    nlohmann::json m_graph, m_saved;
    std::deque<nlohmann::json> m_undo,m_redo;
    uint64_t m_revision=1,m_compiledRevision=0;
    Anim::Graph::CompileResult m_compile;
    bool m_gesture=false,m_gestureCommitted=false;

public:
    std::string path;
    Anim::Graph::GraphResolver resolver;
};
}
