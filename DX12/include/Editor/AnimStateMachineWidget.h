#pragma once
#include "Editor/AnimStateMachineView.h"
#include "Editor/AnimGraphDocument.h"
#include "imgui/imgui.h"
#include <cmath>
#include <set>
namespace Editor {
// Production widget: the headless test feeds actual ImGui input into this code.
struct AnimStateMachineWidget {
    AnimStateMachineView view;
    int stateDrag=-1,transitionFrom=-1;
    ImVec2 boxStart{},origin{};bool box=false;
    std::vector<int> stateSelection;
    void Draw(AnimGraphDocument& document,const std::string& machine,std::string& error){

    using Json=nlohmann::json;
    auto Apply=[&](const Json& ops){document.Apply(ops,error);};
    const auto graph=document.Graph();Json node;
    for(const auto& candidate:graph["nodes"])if(candidate.at("id")==machine)node=candidate;
    if(node.is_null() || !node.contains("states")) {ImGui::TextDisabled("Select a StateMachine node");return;}
    auto states=node["states"];auto transitions=node.value("transitions",Json::array());
    const auto start=ImGui::GetCursorScreenPos();const auto size=ImGui::GetContentRegionAvail();origin=start;
    ImGui::InvisibleButton("State Machine Canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);
    const bool hover=ImGui::IsItemHovered();auto& io=ImGui::GetIO();auto& view=this->view;
    if(hover && io.MouseWheel!=0)view.zoom=std::clamp(view.zoom*std::pow(1.15f,io.MouseWheel),.25f,2.5f);
    if(hover && ImGui::IsMouseDragging(2)) {view.pan[0]+=io.MouseDelta.x;view.pan[1]+=io.MouseDelta.y;}
    std::vector<std::array<float,2>> positions;for(size_t i=0;i<states.size();++i) {
        auto position=std::array<float,2>{float(i%3)*200,float(i/3)*110};
        const auto authored=graph.value("editor",Json::object()).value("statePositions",Json::object()).value(machine,Json::object());
        const auto name=states[i]["name"].get<std::string>();if(authored.contains(name))position=authored.at(name).get<std::array<float,2>>();
        positions.push_back(position);
    }
    auto at=[&](int i){return ImVec2(start.x+view.pan[0]+positions[i][0]*view.zoom,start.y+view.pan[1]+positions[i][1]*view.zoom);};
    auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(start,ImVec2(start.x+size.x,start.y+size.y),true);
    for(const auto& transition:transitions) {
        int from=-1,to=-1;for(int i=0;i<states.size();++i) {if(states[i]["name"]==transition["from"])from=i;if(states[i]["name"]==transition["to"])to=i;}
        if(from<0 || to<0)continue;auto a=at(from),b=at(to);a.x+=75*view.zoom;a.y+=30*view.zoom;b.x+=75*view.zoom;b.y+=30*view.zoom;
        draw->AddLine(a,b,IM_COL32(180,180,180,255),2);draw->AddCircleFilled(b,4,IM_COL32(220,200,80,255));
    }
    for(int i=0;i<states.size();++i) {
        const auto a=at(i),b=ImVec2(a.x+150*view.zoom,a.y+60*view.zoom);
        const bool selected=std::find(stateSelection.begin(),stateSelection.end(),i)!=stateSelection.end();
        draw->AddRectFilled(a,b,selected?IM_COL32(70,100,150,255):IM_COL32(45,50,60,255),6);
        draw->AddText(ImVec2(a.x+8,a.y+8),IM_COL32(240,240,240,255),states[i]["name"].get<std::string>().c_str());
    }
    const int hit=view.Hit(io.MousePos.x-start.x,io.MousePos.y-start.y,positions);
    if(hover && ImGui::IsMouseClicked(0)) {
        if(io.KeyShift && hit>=0)transitionFrom=hit;
        else if(hit>=0) {if(!io.KeyCtrl)stateSelection={hit};else stateSelection.push_back(hit);stateDrag=hit;document.BeginGesture();}
        else {box=true;boxStart=io.MousePos;}
    }
    if(transitionFrom>=0 && ImGui::IsMouseDown(0))draw->AddLine(at(transitionFrom),io.MousePos,IM_COL32(230,210,70,255),2);
    if(stateDrag>=0 && ImGui::IsMouseDragging(0)) {
        Json moves=Json::array();for(auto selected:stateSelection)if(selected>=0 && selected<states.size())
            moves.push_back({{"op","state_move"},{"node",machine},{"state",states[selected]["name"]},
                {"pos",{positions[selected][0]+io.MouseDelta.x/view.zoom,positions[selected][1]+io.MouseDelta.y/view.zoom}}});
        if(!moves.empty())Apply(moves);
    }
    if(stateDrag>=0 && ImGui::IsMouseReleased(0)) {stateDrag=-1;document.EndGesture();}
    if(hover && ImGui::IsKeyPressed(ImGuiKey_Delete) && !stateSelection.empty()) {
        std::set<std::string> remove;for(auto selected:stateSelection)if(selected>=0 && selected<states.size()) {
            const auto name=states[selected]["name"].get<std::string>();if(name!=node.value("default",""))remove.insert(name);
        }
        states.erase(std::remove_if(states.begin(),states.end(),[&](const auto& state){return remove.contains(state.at("name").template get<std::string>());}),states.end());
        transitions.erase(std::remove_if(transitions.begin(),transitions.end(),[&](const auto& transition){return remove.contains(transition.at("from").template get<std::string>()) || remove.contains(transition.at("to").template get<std::string>());}),transitions.end());
        Apply(Json::array({{{"op","set"},{"node",machine},{"props",{{"states",states},{"transitions",transitions}}}}}));stateSelection.clear();
    }
    if(transitionFrom>=0 && ImGui::IsMouseReleased(0)) {
        if(hit>=0 && hit!=transitionFrom) {
            transitions.push_back({{"from",states[transitionFrom]["name"]},{"to",states[hit]["name"]},{"duration",.15f},{"exitTime",1.f},{"mode","inertialize"}});
            Apply(Json::array({{{"op","set"},{"node",machine},{"props",{{"transitions",transitions}}}}}));
        }transitionFrom=-1;
    }
    if(box) {
        draw->AddRect(boxStart,io.MousePos,IM_COL32(110,170,240,255));
        if(ImGui::IsMouseReleased(0)) {stateSelection=view.Select(boxStart.x-start.x,boxStart.y-start.y,io.MousePos.x-start.x,io.MousePos.y-start.y,positions);box=false;}
    }
    draw->PopClipRect();
    }
};
}
