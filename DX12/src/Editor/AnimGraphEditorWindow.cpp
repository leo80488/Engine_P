#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/AnimGraphDocument.h"
#include "Editor/NodeGraphView.h"
#include "Editor/GraphLayout.h"
#include "Editor/AnimStateMachineWidget.h"
#include "ECS/AnimGraphSystem.h"
#include "ECS/AnimEntityResolve.h"
#include "Anim/Graph/DebugSnapshot.h"
#include "Editor/PreviewStage.h"
#include "Editor/AnimGraphCpuPreview.h"
#include "Renderer/Renderer.h"
#include "System/CommandLine.h"
#include "ECS/AnimationSystem.h"
#include "Resource/AssetFS.h"
#include "System/PathUtil.h"
#include "System/Log.h"
#include "imgui/imgui.h"
#include <cstring>
#include <map>
#include <set>

namespace ed=ax::NodeEditor;
namespace Editor::AnimGraphEditor {
namespace {
using Json=nlohmann::json;
struct Window {
    Window(){document.resolver=[](const std::string& path){std::string text;if(!Resource::AssetFS::Get().ReadFileText(path,text))throw std::runtime_error("SubGraph not found: "+path);return Json::parse(text);};}
    AnimGraphDocument document;
    bool open=false,fit=true,focused=false;
    void* context=nullptr;
    std::string error,selected,machine,savedFileBytes;
    uint64_t pushed=0,compileRevision=0;
    uint32_t drawnFrames=0;
    double editTime=0;
    std::vector<char> details;
    char path[1024]="asset/New.ianimgraph",palette[64]="RefPose";
    Entity watch=NullEntity;
    PreviewStageHost stage{{PreviewStageTag::kAnimGraphEditor,2,"[ANIMGRAPH_PREVIEW]","[AnimationGraph] ","__AnimGraphCamera"}};
    bool previewWanted=false,previewPlay=false,previewStep=false,previewRespawn=false,selectPreview=false;
    uint32_t harnessFrame=0,mainHash=0;Json harness;bool harnessLoaded=false;
    uint32_t stageEpoch=~0u;uint64_t previewRevision=0;
    World* stageWorld=nullptr;Renderer* stageRenderer=nullptr;
    char subject[1024]="asset/Content/Prefab/LocoDemo_8Way.legacy.ipfb",previewSet[1024]="";
    bool cpuMode=false;
    AnimGraphCpuPreview cpu;
    char cpuMesh[1024]="asset/Content/endfield/Rossi/Rossi.meshlib",cpuSkeleton[1024]="asset/Content/endfield/Rossi/Rossi.iskel";
    float previewDt=1.f/60;
    Json previewState=Json::object();std::deque<Json> runtimeCommands;
    AnimStateMachineWidget stateView;
    std::map<std::string,ImVec2> pushedPositions;
} window;
std::vector<std::string> Inputs(const Json& node) {
    const auto type=node.value("type","");std::vector<std::string> pins;
    if(type=="Output" || type=="CachedPose" || type=="Inertialization")pins={"pose"};
    else if(type=="Slot")pins={"source"};
    else if(type=="TimeScale")pins={"pose","rate"};
    else if(type=="LayeredBlend")pins={"base"};
    else if(type=="AimOffset")pins={"base","x","y","weight"};
    else if(type=="Clip")pins={"rate"};
    else if(type=="BlendByBool")pins={"a","b","condition"};
    else if(type=="Blend2" || type=="ApplyAdditive")pins={"a","b","alpha"};
    else if(type=="Not" || type=="Damp" || type=="WrapAngle")pins={"a"};
    else if(type=="Math" || type=="Compare" || type=="And" || type=="Or" || type=="Xor" || type=="DeltaAngle")pins={"a","b"};
    if(node.contains("in"))for(auto it=node["in"].begin();it!=node["in"].end();++it)
        if(std::find(pins.begin(),pins.end(),it.key())==pins.end())pins.push_back(it.key());
    return pins;
}
bool TypedValue(const char* label,const std::string& type,Json& value){
    if(type=="bool" || type=="trigger"){bool v=value.is_boolean()?value.get<bool>():false;if(ImGui::Checkbox(label,&v)){value=v;return true;}}
    else if(type=="int"){int v=value.is_number_integer()?value.get<int>():0;if(ImGui::InputInt(label,&v)){value=v;return true;}}
    else if(type=="id"){char v[256]{};std::snprintf(v,sizeof(v),"%s",value.is_string()?value.get<std::string>().c_str():"");if(ImGui::InputText(label,v,sizeof(v))){value=v;return true;}}
    else if(type=="vector3"){std::array<float,3> v{};if(value.is_array() && value.size()==3)v=value.get<std::array<float,3>>();if(ImGui::DragFloat3(label,v.data(),.01f)){value=v;return true;}}
    else{float v=value.is_number()?value.get<float>():0;if(ImGui::DragFloat(label,&v,.01f)){value=v;return true;}}
    return false;
}
void Apply(Json ops) { if(!window.document.Apply(ops,window.error))return;window.error.clear();window.editTime=ImGui::GetCurrentContext()?ImGui::GetTime():0; }
bool Save() {
    const auto text=window.document.Text();
    if(window.document.path.empty())window.document.path=window.path;
    std::string disk;
    if(Resource::AssetFS::Get().ReadFileText(window.document.path,disk) && disk!=window.savedFileBytes){window.error="Graph changed on disk. Reload it or save to a new path.";return false;}
    if(!Sys::WriteFileBytesAtomic(window.document.path,text.data(),text.size())) {window.error="Could not save graph";return false;}
    window.savedFileBytes=text;window.document.Saved();LOG_INFO("[ANIMGRAPH_EDITOR] saved %s",window.document.path.c_str());return true;
}
void Select(const std::string& id) {
    if(window.selected==id)return;window.selected=id;window.details.clear();
    for(const auto& node:window.document.Graph()["nodes"])if(node.at("id")==id) {
        auto text=node.dump(2);window.details.resize(std::max<size_t>(16384,text.size()+4096));std::memcpy(window.details.data(),text.data(),text.size());
        if(node.value("type","")=="StateMachine")window.machine=id;
    }
}
void Canvas(const Anim::Graph::Instance* live) {
    if(!window.context)window.context=NodeGraphView::CreateContext();
    ed::SetCurrentEditor(static_cast<ed::EditorContext*>(window.context));
    const auto graph=window.document.Graph();const auto& nodes=graph["nodes"];
    std::map<std::string,uint32_t> ids;std::map<uint32_t,std::string> names;
    for(const auto& node:nodes) {auto name=node.at("id").get<std::string>();auto id=Anim::Graph::Hash(name);ids[name]=id;names[id]=name;}
    std::map<std::string,std::vector<std::string>> inputs;for(const auto& node:nodes)inputs[node.at("id")]=Inputs(node);
    ed::Begin("Animation Graph Canvas");
    if(window.pushed!=window.document.Revision()) {
        std::vector<std::vector<size_t>> edges(nodes.size());
        std::map<std::string,size_t> index;for(size_t i=0;i<nodes.size();++i)index[nodes[i].at("id")]=i;
        for(size_t i=0;i<nodes.size();++i)if(nodes[i].contains("in"))for(const auto& pin:nodes[i]["in"])
            if(pin.is_string() && index.contains(pin.get<std::string>()))edges[i].push_back(index.at(pin.get<std::string>()));
        const auto positions=GraphLayout::Layers(edges);
        for(size_t i=0;i<nodes.size();++i) {
            const auto name=nodes[i].at("id").get<std::string>();auto position=positions[i];
            if(graph.contains("editor") && graph["editor"].contains("positions") && graph["editor"]["positions"].contains(name))
                position=graph["editor"]["positions"][name].get<std::array<float,2>>();
            ed::SetNodePosition(ed::NodeId(ids[name]),ImVec2(position[0],position[1]));
            window.pushedPositions[name]=ImVec2(position[0],position[1]);
        }window.pushed=window.document.Revision();
    }
    for(const auto& node:nodes) {
        const auto name=node.at("id").get<std::string>();const auto id=ids[name];
        float weight=0;if(live)for(size_t i=0;i<live->states.size();++i)if(live->definition->nodes[i].id==name)weight=live->states[i].weight;
        ed::PushStyleColor(ed::StyleColor_NodeBorder,weight>Anim::Graph::kRelevanceEps?ImVec4(.3f,.9f,.5f,1):ImVec4(.3f,.3f,.3f,1));
        ed::BeginNode(ed::NodeId(id));
        const auto type=node.at("type").get<std::string>();const auto& pins=inputs[name];
        const float outWidth=ImGui::CalcTextSize("Out o").x;
        float inputWidth=0;
        for(const auto& pin:pins)inputWidth=std::max(inputWidth,ImGui::CalcTextSize(("o "+pin).c_str()).x);
        const float nodeWidth=std::max({180.f,ImGui::CalcTextSize(name.c_str()).x,ImGui::CalcTextSize(type.c_str()).x,inputWidth+48.f+outWidth});
        ImGui::Text("%s",name.c_str());ImGui::TextDisabled("%s",type.c_str());
        ImGui::BeginGroup();
        if(pins.empty())ImGui::Dummy(ImVec2(1,ImGui::GetTextLineHeight()));
        for(size_t i=0;i<pins.size();++i) {
            ed::BeginPin(NodeGraphView::Pin(id,static_cast<uint16_t>(i),false),ed::PinKind::Input);
            ed::PinPivotAlignment(ImVec2(0,.5f));
            ImGui::Text("o %s",pins[i].c_str());ed::EndPin();
        }
        ImGui::EndGroup();ImGui::SameLine(0,nodeWidth-std::max(1.f,inputWidth)-outWidth);
        ed::BeginPin(NodeGraphView::Pin(id,0,true),ed::PinKind::Output);
        ed::PinPivotAlignment(ImVec2(1,.5f));
        ImGui::Text("Out o");ed::EndPin();
        if(live)ImGui::Text("Weight %.3f",weight);ed::EndNode();ed::PopStyleColor();
    }
    for(const auto& node:nodes) {
        const auto name=node.at("id").get<std::string>();const auto& pins=inputs[name];
        if(!node.contains("in"))continue;
        for(size_t i=0;i<pins.size();++i)if(node["in"].contains(pins[i]) && node["in"][pins[i]].is_string()) {
            const auto source=node["in"][pins[i]].get<std::string>();if(!ids.contains(source))continue;
            ed::Link(NodeGraphView::Link(ids[name],static_cast<uint16_t>(i)),NodeGraphView::Pin(ids[source],0,true),NodeGraphView::Pin(ids[name],static_cast<uint16_t>(i),false));
        }
    }
    if(ed::BeginCreate()) {
        ed::PinId a,b;if(ed::QueryNewLink(&a,&b) && a && b) {
            auto from=a.Get(),to=b.Get();if(!(from&(1ull<<15)))std::swap(from,to);
            const auto fi=static_cast<uint32_t>(from>>16),ti=static_cast<uint32_t>(to>>16);
            const auto pin=static_cast<uint16_t>(to&0x7fff);
            if((from&(1ull<<15)) && !(to&(1ull<<15)) && names.contains(fi) && names.contains(ti) && pin<inputs[names[ti]].size()) {
                auto op=Json::array({{{"op","connect"},{"from",names[fi]},{"to",names[ti]},{"pin",inputs[names[ti]][pin]}}});
                AnimGraphDocument probe;probe.resolver=window.document.resolver;std::string why;probe.Load(window.document.Text(),why);
                if(probe.Apply(op,why)) {if(ed::AcceptNewItem())Apply(op);}else ed::RejectNewItem(ImVec4(1,.2f,.2f,1),2);
            }else ed::RejectNewItem();
        }ed::EndCreate();
    }
    if(ed::BeginDelete()) {
        ed::LinkId link;while(ed::QueryDeletedLink(&link)) {
            const auto value=link.Get();const auto id=static_cast<uint32_t>((value&~(1ull<<62))>>16);const auto pin=static_cast<uint16_t>(value&0xffff);
            if(names.contains(id) && pin<inputs[names[id]].size() && ed::AcceptDeletedItem())Apply(Json::array({{{"op","disconnect"},{"to",names[id]},{"pin",inputs[names[id]][pin]}}}));
        }
        ed::NodeId node;while(ed::QueryDeletedNode(&node))if(names.contains(static_cast<uint32_t>(node.Get())) && names[static_cast<uint32_t>(node.Get())]!=graph.at("root").get<std::string>() && ed::AcceptDeletedItem())
            Apply(Json::array({{{"op","remove"},{"node",names[static_cast<uint32_t>(node.Get())]}}}));
        ed::EndDelete();
    }
    ed::NodeId selected;if(ed::GetSelectedNodes(&selected,1)>0 && names.contains(static_cast<uint32_t>(selected.Get())))Select(names[static_cast<uint32_t>(selected.Get())]);
    if(window.fit){ed::NavigateToContent(0);window.fit=false;}
    if(!ImGui::IsMouseDown(0)) {
        Json moves=Json::array();for(const auto& [name,id]:ids) {
            const auto p=ed::GetNodePosition(ed::NodeId(id));
            const auto previous=window.pushedPositions[name];
            if(previous.x!=p.x || previous.y!=p.y)moves.push_back({{"op","move"},{"node",name},{"pos",{p.x,p.y}}});
        }if(!moves.empty())Apply(moves);
    }
    ed::End();ed::SetCurrentEditor(nullptr);
    ++window.drawnFrames;
}
void StateView(){window.stateView.Draw(window.document,window.machine,window.error);}
}
bool Open(const std::string& path) {
    if(window.document.Dirty() && window.document.path!=path){window.error="Save or revert the current graph before opening another";return false;}
    std::vector<uint8_t> bytes;if(!Resource::AssetFS::Get().ReadFile(path,bytes)){window.error="Could not read graph";return false;}
    if(!window.document.Load(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),window.error))return false;
    window.savedFileBytes.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    window.document.path=path;std::snprintf(window.path,sizeof(window.path),"%s",path.c_str());window.open=true;window.fit=true;window.selected.clear();window.machine.clear();
    const auto& compiled=window.document.Compile();LOG_INFO("[ANIMGRAPH_EDITOR] open %s compiled=%d diagnostics=%zu",path.c_str(),compiled.Ok()?1:0,compiled.diagnostics.size());return true;
}
void Shutdown(){window.stage.TearDown(window.stageRenderer,window.stageWorld,"shutdown");NodeGraphView::DestroyContext(window.context);window.context=nullptr;}
bool ConsumeFocusedLastDraw(){const bool focused=window.open && window.focused;window.focused=false;return focused;}
std::string Control(const std::string& request) {
    try {
        const auto command=Json::parse(request);const auto op=command.value("action","state");
        if(op=="open" && !Open(command.at("path")))return Json{{"ok",false},{"error",window.error}}.dump();
        if(op=="new") {if(window.document.Dirty()){window.open=true;window.error="Save or revert the current graph before creating another";return Json{{"ok",false},{"error",window.error}}.dump();}window.document.New();window.savedFileBytes.clear();window.document.path=command.value("path",std::string(window.path));std::snprintf(window.path,sizeof(window.path),"%s",window.document.path.c_str());window.open=true;window.fit=true;window.selected.clear();window.machine.clear();window.details.clear();window.error.clear();}
        if(op=="edit" && !window.document.Apply(command.at("ops"),window.error))return Json{{"ok",false},{"error",window.error}}.dump();
        if(op=="undo") {window.document.Undo();window.error.clear();}if(op=="redo") {window.document.Redo();window.error.clear();}
        if(op=="save" && !Save())return Json{{"ok",false},{"error",window.error}}.dump();
        if(op=="revert" && !Open(window.document.path))return Json{{"ok",false},{"error",window.error}}.dump();
        if(op=="copy")return Json{{"ok",true},{"fragment",window.document.Copy(command.value("nodes",std::vector<std::string>{window.selected}))}}.dump();
        if(op=="paste" && !window.document.Paste(command.at("fragment"),window.error))return Json{{"ok",false},{"error",window.error}}.dump();
        if(op=="watch")window.watch=command.at("entity").get<Entity>();
        if(op=="preview") {
            window.previewWanted=command.value("enabled",true);window.selectPreview=true;
            if(command.contains("subject")){std::snprintf(window.subject,sizeof(window.subject),"%s",command["subject"].get<std::string>().c_str());window.previewRespawn=true;}
            if(command.contains("animSet")){std::snprintf(window.previewSet,sizeof(window.previewSet),"%s",command["animSet"].get<std::string>().c_str());window.previewRevision=0;}
            if(command.contains("cpu")){const bool value=command["cpu"].get<bool>();if(value!=window.cpuMode){window.cpuMode=value;window.previewRevision=0;window.previewRespawn=true;}}
            if(command.contains("mesh")){std::snprintf(window.cpuMesh,sizeof(window.cpuMesh),"%s",command["mesh"].get<std::string>().c_str());window.previewRevision=0;}
            if(command.contains("skeleton")){std::snprintf(window.cpuSkeleton,sizeof(window.cpuSkeleton),"%s",command["skeleton"].get<std::string>().c_str());window.previewRevision=0;}
            if(command.contains("playing"))window.previewPlay=command["playing"].get<bool>();
            if(command.value("step",false))window.previewStep=true;
            if(command.contains("dt")){const float value=command["dt"].get<float>();if(!std::isfinite(value) || value<=0 || value>.25f)throw std::runtime_error("Preview dt must be in (0,.25]");window.previewDt=value;}
        }
        if(op=="rewind" && !command.contains("index") && !command.contains("frame"))throw std::runtime_error("Rewind requires a recorded frame index");
        if(op=="parameter" && (!command.contains("name") || !command.contains("value")))throw std::runtime_error("Parameter requires name and value");
        if(op=="record" || op=="rewind" || op=="parameter") {
            if(window.runtimeCommands.size()>=64)throw std::runtime_error("Preview command queue is full");window.runtimeCommands.push_back(command);
        }
        const auto& compiled=window.document.Compile();Json diagnostics=Json::array();
        for(const auto& error:compiled.diagnostics)diagnostics.push_back({{"node",error.node},{"message",error.message},{"warning",error.warning}});
        return Json{{"ok",true},{"open",window.open},{"drawnFrames",window.drawnFrames},{"path",window.document.path},{"dirty",window.document.Dirty()},{"revision",window.document.Revision()},
            {"compiled",compiled.Ok()},{"diagnostics",diagnostics},{"graph",window.document.Graph()},{"preview",window.previewState}}.dump();
    }catch(const std::exception& e){return Json{{"ok",false},{"error",e.what()}}.dump();}
}
void ProcessPendingActions(World* world,Renderer* renderer,Resource::AssetManager* assets,Resource::AnimationClipSystem* clips,bool editMode) {
    if(!world || !renderer || !assets)return;
    ++window.harnessFrame;
    auto sceneHash=[&] {
        uint32_t hash=2166136261u;
        world->ForEach<LocalTransform>([&](Entity e,LocalTransform& transform) {
            if(world->HasComponent<SceneTransientTag>(e))return;
            const auto* bytes=reinterpret_cast<const uint8_t*>(&transform);
            hash=(hash^e)*16777619u;for(size_t i=0;i<sizeof(transform);++i)hash=(hash^bytes[i])*16777619u;
        });return hash;
    };
    if(!window.harnessLoaded) {
        window.harnessLoaded=true;const auto path=Sys::CommandLine::Get().Get("-animgraph_preview_script");
        if(!path.empty()) {std::string text;if(Resource::AssetFS::Get().ReadFileText(path,text))try{window.harness=Json::parse(text);}catch(const std::exception& e){LOG_ERROR("[ANIMGRAPH_PREVIEW] invalid harness: %s",e.what());}}
    }
    if(window.harness.is_array())for(const auto& command:window.harness)if(command.value("frame",0u)==window.harnessFrame) {
        const auto op=command.value("action","");
        if(op=="baseline")window.mainHash=sceneHash();
        else if(op=="probe") {
            const auto& state=window.previewState;bool ok=true;
            for(auto it=command.begin();it!=command.end();++it) {
                if(it.key()=="action" || it.key()=="frame")continue;
                if(it.key()=="editMode")ok=ok && editMode==it.value().get<bool>();
                else if(it.key()=="mainUnchanged")ok=ok && sceneHash()==window.mainHash;
                else if(it.key()=="minRecordedFrames")ok=ok && state.value("recordedFrames",0)>=it.value().get<int>();
                else if(it.key()=="minRenders")ok=ok && state.value("renders",0u)>=it.value().get<uint32_t>();
                else if(it.key()=="rewindMatches")ok=ok && state.value("rewindMatches",false);
                else if(it.key()=="minTriangles")ok=ok && state.value("triangles",size_t(0))>=it.value().get<size_t>();
                else if(it.key()=="state") {
                    bool found=false;
                    if(state.contains("snapshot"))for(const auto& node:state["snapshot"]["nodes"])
                        if(node.value("id","")==it.value().at("machine").get<std::string>())found=node.value("state","")==it.value().at("name").get<std::string>() && node.value("transitioning",false)==it.value().value("transitioning",false);
                    ok=ok && found;
                }
                else ok=ok && state.contains(it.key()) && state[it.key()]==it.value();
            }
            if(ok)LOG_INFO("[ANIMGRAPH_PREVIEW] probe frame=%u PASS edit=%d mainUnchanged=%d up=%d ready=%d recorded=%d renders=%u",window.harnessFrame,editMode?1:0,sceneHash()==window.mainHash?1:0,state.value("up",false)?1:0,state.value("ready",false)?1:0,state.value("recordedFrames",0),state.value("renders",0u));
            else LOG_ERROR("[ANIMGRAPH_PREVIEW] probe frame=%u FAIL state=%s",window.harnessFrame,state.dump().c_str());
        }else Control(command.dump());
    }

    if(window.stageWorld!=world || window.stageEpoch!=world->Epoch()) {
        window.stage.ForgetEntities();window.stageWorld=world;window.stageEpoch=world->Epoch();window.previewRevision=0;
    }
    window.stageRenderer=renderer;
    if(!window.open || !window.previewWanted || Sys::CommandLine::Get().Has("-animgraph_preview_off")) {
        if(window.stage.Up())window.stage.TearDown(renderer,world,"preview closed");
        window.cpu.instance.reset();window.previewRevision=0;
        window.previewState={{"up",false}};window.runtimeCommands.clear();return;
    }
    if(window.previewRespawn){window.stage.TearDown(renderer,world,"subject changed");window.previewRespawn=false;window.previewRevision=0;}
    if(window.cpuMode) {
        if(window.stage.Up())window.stage.TearDown(renderer,world,"CPU preview selected");
        try {
            const auto& compiled=window.document.Compile();
            if(compiled.Ok() && (window.previewRevision!=window.document.Revision() || !window.cpu.instance)) {
                window.cpu.Load(compiled.definition,window.cpuMesh,window.cpuSkeleton,window.previewSet);
                window.previewRevision=window.document.Revision();
            }
            auto* instance=window.cpu.instance.get();
            for(const auto& command:window.runtimeCommands)if(instance) {
                const auto op=command.value("action","");
                if(op=="record"){if(command.value("enabled",true))instance->StartRecording(command.value("capacity",size_t(600)));else instance->StopRecording();}
                if(op=="rewind"){window.previewPlay=false;if(!instance->Rewind(window.cpu.clips,command.value("index",command.value("frame",size_t(0)))))throw std::runtime_error("CPU recording rewind failed");}
                if(op=="parameter") {
                    const std::string name=command.at("name");const int index=instance->definition->FindParam(name);
                    if(index<0)throw std::runtime_error("Unknown CPU preview parameter: "+name);
                    const auto type=instance->definition->params[index].type;
                    if(type==Anim::Graph::ParamType::Trigger)instance->Latch(instance->params,1ull<<index);
                    else instance->params[index]=Anim::Graph::ReadParamValue(command.at("value"),type);
                }
            }
            window.runtimeCommands.clear();
            const float step=window.previewPlay || window.previewStep?window.previewDt:0;
            if(instance){instance->Latch(instance->params,0);if(step>0)instance->holdRecordedPose=false;window.cpu.Tick(step);}
            window.previewStep=false;
            window.previewState={{"up",bool(instance)},{"ready",bool(instance)},{"cpu",true},{"view",0},{"entities",0},{"playing",window.previewPlay},{"renders",window.cpu.renders},{"triangles",window.cpu.triangles}};
            if(instance){window.previewState["snapshot"]=Anim::Graph::DebugSnapshot(*instance);window.previewState["rewindMatches"]=instance->holdRecordedPose;window.previewState["recordedFrames"]=instance->RecordedFrames().size();window.previewState["recording"]=instance->Recording();}
        }catch(const std::exception& e){window.error=e.what();window.previewState={{"up",false},{"cpu",true},{"error",window.error}};window.runtimeCommands.clear();}
        return;
    }
    if(window.stage.Up() && !window.stage.Validate(*world)){window.stage.TearDown(renderer,world,"subject destroyed");window.previewRevision=0;}
    if(!window.stage.Up()) {
        if(!window.stage.Spawn(*world,*renderer,*assets,clips,window.subject,window.error)) {window.previewState={{"up",false},{"error",window.error}};return;}
        window.stage.Pin(*renderer);window.previewRevision=0;
    }
    const Entity entity=Anim::ResolveAnimEntityOrSelf(*world,window.stage.Skeleton());auto* system=renderer->GetAnimGraphSystem();
    if(window.previewRevision!=window.document.Revision()) {
        const auto& compiled=window.document.Compile();
        if(compiled.Ok()) {
            const std::string path="preview://animation-graph-editor";system->SetPreviewDefinition(path,compiled.definition);
            AnimGraphComponent graph;graph.graphPath=path;graph.animSetPath=window.previewSet;graph.previewDt=0;
            world->AddComponent<AnimGraphComponent>(entity,std::move(graph));
            if(auto* animation=world->GetComponent<AnimationComponent>(entity)){animation->externalTime=false;animation->paused=false;}
            window.previewRevision=window.document.Revision();
        }
    }
    auto* instance=system->GetInstance(entity);
    for(const auto& command:window.runtimeCommands)if(instance)try {
        const auto op=command.value("action","");
        if(op=="record") {if(command.value("enabled",true))instance->StartRecording(command.value("capacity",size_t(600)));else instance->StopRecording();}
        if(op=="rewind") {window.previewPlay=false;if(!system->Rewind(entity,command.value("index",command.value("frame",size_t(0)))))window.error="Recording rewind failed or stale definition";}
        if(op=="parameter") {
            const std::string name=command.at("name");const int index=instance->definition->FindParam(name);
            if(index<0){window.error="Unknown preview parameter: "+name;continue;}
            const auto type=instance->definition->params[index].type;
            if(type==Anim::Graph::ParamType::Trigger)system->Trigger(*world,entity,name);
            else system->Set(*world,entity,name,Anim::Graph::ReadParamValue(command.at("value"),type));
        }
    }
    catch(const std::exception& e){window.error=e.what();}
    window.runtimeCommands.clear();
    const float step=window.previewPlay || window.previewStep?window.previewDt:0;
    if(auto* graph=world->GetComponent<AnimGraphComponent>(entity))graph->previewDt=step;
    if(instance && step>0)instance->holdRecordedPose=false;
    window.previewStep=false;
    if(window.stage.EnsureView(*renderer)) {
        window.stage.NegotiateSize(*renderer,480,360);window.stage.EnsureCamera(*world);
        const float eye[]={0,window.stage.OriginY()+1.0f,-4.f},forward[]={0,-.08f,1.f};
        window.stage.PlaceCamera(*world,eye,forward);window.stage.Request(*renderer);
    }
    window.previewState={{"up",true},{"entity",entity},{"root",window.stage.Root()},{"entities",window.stage.Entities().size()},
        {"originY",window.stage.OriginY()},{"pinned",window.stage.Pinned()},{"view",window.stage.View()},{"playing",window.previewPlay},
        {"ready",instance && system->OwnsPose(entity)},{"renders",renderer->GetPreviewViewRenderCount(window.stage.View())}};
    if(instance) {window.previewState["rewindMatches"]=instance->holdRecordedPose;window.previewState["snapshot"]=Anim::Graph::DebugSnapshot(*instance);window.previewState["recordedFrames"]=instance->RecordedFrames().size();window.previewState["recording"]=instance->Recording();}
}
void Draw(World* world,AnimGraphSystem* system) {
    if(!window.open || Sys::CommandLine::Get().Has("-animgraph_editor_off"))return;
    ImGui::SetNextWindowSize(ImVec2(1100,720),ImGuiCond_FirstUseEver);
    const bool visible=ImGui::Begin("Animation Graph",&window.open,ImGuiWindowFlags_MenuBar);
    window.focused=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if(!visible) {ImGui::End();return;}
    if(ImGui::BeginMenuBar()) {
        if(ImGui::MenuItem("Save","Ctrl+S"))Save();if(ImGui::MenuItem("Undo","Ctrl+Z"))window.document.Undo();if(ImGui::MenuItem("Redo","Ctrl+Y"))window.document.Redo();
        if(ImGui::MenuItem("Copy","Ctrl+C",false,!window.selected.empty()))ImGui::SetClipboardText(window.document.Copy({window.selected}).dump().c_str());
        if(ImGui::MenuItem("Paste","Ctrl+V"))try{window.document.Paste(Json::parse(ImGui::GetClipboardText()),window.error);}catch(const std::exception& e){window.error=e.what();}
        if(ImGui::MenuItem("Fit"))window.fit=true;
        ImGui::EndMenuBar();
    }
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl) {
        if(!ImGui::GetIO().WantTextInput){
            if(ImGui::IsKeyPressed(ImGuiKey_C) && !window.selected.empty())ImGui::SetClipboardText(window.document.Copy({window.selected}).dump().c_str());
            if(ImGui::IsKeyPressed(ImGuiKey_V))try{window.document.Paste(Json::parse(ImGui::GetClipboardText()),window.error);}catch(const std::exception& e){window.error=e.what();}
        }
        if(ImGui::IsKeyPressed(ImGuiKey_S))Save();
        if(!ImGui::GetIO().WantTextInput){if(ImGui::IsKeyPressed(ImGuiKey_Z)){if(ImGui::GetIO().KeyShift)window.document.Redo();else window.document.Undo();}if(ImGui::IsKeyPressed(ImGuiKey_Y))window.document.Redo();}
    }
    ImGui::Text("%s%s",window.document.path.c_str(),window.document.Dirty()?" *":"");
    const float width=std::max(300.f,ImGui::GetContentRegionAvail().x-330);
    ImGui::BeginChild("Graph",ImVec2(width,0));
    if(ImGui::BeginTabBar("Graph Views")) {
        if(ImGui::BeginTabItem("Pose Graph")){Canvas(world && system?system->GetInstance(Anim::ResolveAnimEntity(*world,window.watch)):nullptr);ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("State Machine")){StateView();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Preview",nullptr,window.selectPreview?ImGuiTabItemFlags_SetSelected:0)) {
            window.selectPreview=false;
            if(ImGui::Checkbox("CPU Preview",&window.cpuMode)){window.previewRespawn=true;window.previewRevision=0;}
            if(window.cpuMode){if(ImGui::InputText("Mesh",window.cpuMesh,sizeof(window.cpuMesh)))window.previewRevision=0;if(ImGui::InputText("Skeleton",window.cpuSkeleton,sizeof(window.cpuSkeleton)))window.previewRevision=0;}
            else ImGui::InputText("Subject",window.subject,sizeof(window.subject));
            if(ImGui::InputText("Animation Set",window.previewSet,sizeof(window.previewSet)))window.previewRevision=0;
            if(ImGui::BeginDragDropTarget()) {
                if(const auto* payload=ImGui::AcceptDragDropPayload("IANIMSET_PATH")) {
                    std::snprintf(window.previewSet,sizeof(window.previewSet),"%s",Sys::RelativizeAssetPath(static_cast<const char*>(payload->Data)).c_str());window.previewRevision=0;
                }
                ImGui::EndDragDropTarget();
            }
            if(window.previewSet[0] && ImGui::Button("Edit Animation Set"))Editor::AnimSetEditor::Open(window.previewSet);
            if(ImGui::Button("Load Preview")){window.previewWanted=true;window.previewRespawn=true;}
            ImGui::SameLine();if(ImGui::Button("Close Preview"))window.previewWanted=false;
            ImGui::Checkbox("Playing",&window.previewPlay);ImGui::SameLine();if(ImGui::Button("Step"))window.previewStep=true;
            if(ImGui::Button("Record"))window.runtimeCommands.push_back({{"action","record"},{"enabled",true}});
            ImGui::SameLine();if(ImGui::Button("Stop Recording"))window.runtimeCommands.push_back({{"action","record"},{"enabled",false}});
            if(window.previewState.contains("recordedFrames")) {
                static int s_frame=0;const int count=window.previewState["recordedFrames"].get<int>();
                if(count>0 && ImGui::SliderInt("Recorded Frame",&s_frame,0,count-1))window.runtimeCommands.push_back({{"action","rewind"},{"index",s_frame}});
            }
            if(window.cpuMode && window.cpu.instance)window.cpu.Draw(ImVec2(std::max(200.f,ImGui::GetContentRegionAvail().x),360));
            if(window.stageRenderer && window.stage.Up()) {
                const auto srv=window.stageRenderer->GetPreviewSrv(window.stage.View());
                if(srv)ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)),ImVec2(480,360));
            }
            const auto* previewInstance=window.cpuMode?window.cpu.instance.get():world && system?system->GetInstance(Anim::ResolveAnimEntityOrSelf(*world,window.stage.Skeleton())):nullptr;
            if(previewInstance)for(size_t i=0;i<previewInstance->definition->params.size();++i) {
                const auto& declaration=previewInstance->definition->params[i];
                if(declaration.type==Anim::Graph::ParamType::Trigger){if(ImGui::Button(declaration.name.c_str()))window.runtimeCommands.push_back({{"action","parameter"},{"name",declaration.name},{"value",true}});continue;}
                const auto& current=previewInstance->params[i];static const char* kNames[]={"float","int","bool","trigger","vector3","id"};Json value;
                if(declaration.type==Anim::Graph::ParamType::Vector3)value=current.vector;
                else if(declaration.type==Anim::Graph::ParamType::Bool)value=current.Bool();
                else if(declaration.type==Anim::Graph::ParamType::Int || declaration.type==Anim::Graph::ParamType::Id)value=int32_t(current.integer);
                else value=current.Float();
                if(declaration.type==Anim::Graph::ParamType::Id){int id=int32_t(current.integer);if(ImGui::InputInt(declaration.name.c_str(),&id))window.runtimeCommands.push_back({{"action","parameter"},{"name",declaration.name},{"value",id}});}
                else if(TypedValue(declaration.name.c_str(),kNames[int(declaration.type)],value))window.runtimeCommands.push_back({{"action","parameter"},{"name",declaration.name},{"value",value}});
            }
            ImGui::Text("Preview %s, recorded frames %d",window.previewState.value("ready",false)?"ready":"loading",window.previewState.value("recordedFrames",0));ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("Graph Details",ImVec2(0,0));
    if(ImGui::CollapsingHeader("Add Node",ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kTypes[]={"RefPose","Clip","Blend2","BlendByBool","BlendByInt","BlendSpace1D","BlendSpace2D","LayeredBlend","ApplyAdditive","StateMachine","Slot","CachedPose","TimeScale","Inertialization","Param","Constant","Compare","Math","Not","And","Or","Damp","WrapAngle","DeltaAngle","Remap","Select","ClipCurve","SubGraph","AimOffset"};
        if(ImGui::BeginCombo("Type",window.palette)){for(auto type:kTypes)if(ImGui::Selectable(type))std::snprintf(window.palette,sizeof(window.palette),"%s",type);ImGui::EndCombo();}
        if(ImGui::Button("Add")) {std::set<std::string> ids;for(const auto& node:window.document.Graph()["nodes"])ids.insert(node.at("id"));int suffix=1;std::string id;do{id=std::string(window.palette)+std::to_string(suffix++);}while(ids.contains(id));
            Apply(Json::array({{{"op","add"},{"id",id},{"type",window.palette}}}));Select(id);}
    }
    if(ImGui::CollapsingHeader("Details",ImGuiTreeNodeFlags_DefaultOpen) && !window.details.empty()) {
        ImGui::InputTextMultiline("##Node",window.details.data(),window.details.size(),ImVec2(-1,230));
        if(ImGui::Button("Apply Node Settings"))try {
            auto node=Json::parse(window.details.data());node.erase("id");Apply(Json::array({{{"op","set"},{"node",window.selected},{"props",node}}}));
        }catch(const std::exception& e){window.error=e.what();}
    }
    if(ImGui::CollapsingHeader("Parameters")){
        auto graph=window.document.Graph();auto& declarations=graph["parameters"];bool changed=false;
        for(size_t i=0;i<declarations.size();++i){ImGui::PushID(static_cast<int>(i));auto& p=declarations[i];
            char name[256]{};std::snprintf(name,sizeof(name),"%s",p.value("name","").c_str());if(ImGui::InputText("Name",name,sizeof(name))){const auto old=p.value("name","");p["name"]=name;for(auto& n:graph["nodes"])if(n.value("type","")=="Param" && n.value("param","")==old)n["param"]=name;changed=true;}
            auto type=p.value("type","float");if(ImGui::BeginCombo("Type",type.c_str())){for(const char* t:{"float","int","bool","trigger","vector3","id"})if(ImGui::Selectable(t,type==t)){p["type"]=t;p.erase("default");type=t;changed=true;}ImGui::EndCombo();}
            auto value=p.value("default",Json());if(TypedValue("Default",type,value)){p["default"]=value;changed=true;}
            if(type=="trigger"){float hold=p.value("holdSec",0.f);if(ImGui::DragFloat("Hold seconds",&hold,.01f,0,10)){p["holdSec"]=hold;changed=true;}}
            if(ImGui::Button("Remove")){declarations.erase(declarations.begin()+i);changed=true;ImGui::PopID();break;}
            ImGui::Separator();ImGui::PopID();
        }
        if(ImGui::Button("Add Parameter")){std::set<std::string> names;for(const auto& p:declarations)names.insert(p.value("name",""));int i=1;std::string name;do{name="Parameter"+std::to_string(i++);}while(names.contains(name));declarations.push_back({{"name",name},{"type","float"},{"default",0}});changed=true;}
        if(changed)Apply(Json::array({{{"op","replace"},{"graph",graph}}}));
    }
    for(const auto field:{"slots","masks","syncGroups"})if(ImGui::CollapsingHeader(field)) {
        auto graph=window.document.Graph();auto values=graph.value(field,Json::array());ImGui::PushID(field);
        for(size_t i=0;i<values.size();++i){ImGui::PushID(static_cast<int>(i));char entry[4096]{};std::snprintf(entry,sizeof(entry),"%s",values[i].dump().c_str());
            if(ImGui::InputText("Value",entry,sizeof(entry),ImGuiInputTextFlags_EnterReturnsTrue))try{values[i]=Json::parse(entry);graph[field]=values;Apply(Json::array({{{"op","replace"},{"graph",graph}}}));}catch(const std::exception& e){window.error=e.what();}
            if(ImGui::Button("Remove")){values.erase(values.begin()+i);graph[field]=values;Apply(Json::array({{{"op","replace"},{"graph",graph}}}));ImGui::PopID();break;}ImGui::PopID();}
        static char s_entry[1024]="{}";ImGui::InputText("New",s_entry,sizeof(s_entry));
        if(ImGui::Button("Add Entry"))try{values.push_back(Json::parse(s_entry));graph[field]=values;Apply(Json::array({{{"op","replace"},{"graph",graph}}}));}catch(const std::exception& e){window.error=e.what();}ImGui::PopID();
    }
    if(ImGui::CollapsingHeader("Problems",ImGuiTreeNodeFlags_DefaultOpen)) {
        if(ImGui::GetTime()-window.editTime>.15)window.document.Compile();
        const auto& compiled=window.document.Compile();for(const auto& issue:compiled.diagnostics)ImGui::TextWrapped("%s: %s",issue.node.c_str(),issue.message.c_str());
        if(compiled.Ok())ImGui::TextColored(ImVec4(.4f,.9f,.5f,1),"Compiled successfully");
    }
    if(ImGui::CollapsingHeader("Live Entity")) {
        int entity=static_cast<int>(window.watch);if(ImGui::InputInt("Entity",&entity))window.watch=static_cast<Entity>(std::max(0,entity));
        if(world && system)if(const auto* instance=system->GetInstance(Anim::ResolveAnimEntity(*world,window.watch))) {
            ImGui::Text("Frame %u, tasks %zu, poses %zu",instance->frame,instance->tasks.size(),instance->executor.HighWater());
            for(size_t i=0;i<instance->definition->params.size();++i) {
                const auto& param=instance->definition->params[i];const auto& current=instance->params[i];
                if(param.type==Anim::Graph::ParamType::Trigger){if(ImGui::Button(param.name.c_str()))system->Trigger(*world,window.watch,param.name);continue;}
                static const char* kTypes[]={"float","int","bool","trigger","vector3","id"};Json value;
                if(param.type==Anim::Graph::ParamType::Vector3)value=current.vector;
                else if(param.type==Anim::Graph::ParamType::Bool)value=current.Bool();
                else if(param.type==Anim::Graph::ParamType::Int || param.type==Anim::Graph::ParamType::Id)value=int32_t(current.integer);
                else value=current.Float();
                if(param.type==Anim::Graph::ParamType::Id){int id=int32_t(current.integer);if(ImGui::InputInt(param.name.c_str(),&id))system->Set(*world,window.watch,param.name,Anim::Graph::ParamValue::FromInt(id));}
                else if(TypedValue(param.name.c_str(),kTypes[int(param.type)],value))system->Set(*world,window.watch,param.name,Anim::Graph::ReadParamValue(value,param.type));
            }
            for(const auto& node:Anim::Graph::DebugSnapshot(*instance)["nodes"])if(node.contains("state"))ImGui::Text("%s: %s",node["id"].get<std::string>().c_str(),node["state"].get<std::string>().c_str());
            for(const auto& event:instance->debugEvents)ImGui::TextWrapped("%s",event.c_str());
        }
    }
    if(!window.error.empty())ImGui::TextWrapped("%s",window.error.c_str());
    ImGui::EndChild();ImGui::End();
}
}
