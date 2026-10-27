#include "Editor/AttackEditor.h"
#include "Editor/AttackPreviewPose.h"
#include "Editor/CpuSkinRaster.h"
#include "Editor/AttackTimingTracks.h"
#include "Editor/ImGuiWidgets.h"
#include "Gameplay/AttackDefinition.h"
#include "Resource/AnimationLoader.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetFS.h"
#include "Resource/SkeletonAsset.h"
#include "Scene/SceneInstanceLoader.h"
#include "System/PathUtil.h"
#include "imgui/imgui.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace AttackEditor {
using Attack::Json;
using namespace DirectX;
namespace {
struct Preview {
    std::unique_ptr<SkeletonAsset> skeleton=std::make_unique<SkeletonAsset>();
    std::vector<SceneInstanceLoader::PreviewMeshData> meshes;
    ClipAsset clip;
    std::string modelKey,clipPath;
    std::vector<XMFLOAT4X4> global,skin;
    std::vector<XMFLOAT4> rotations;
    Editor::CpuSkinRaster raster;
    bool Load(const Attack::Definition& d) {
        std::string mesh=d.document.value("previewMesh",std::string("asset/Content/endfield/Rossi/Rossi.meshlib"));
        std::string skel=d.document.value("previewSkeleton",std::string("asset/Content/endfield/Rossi/Rossi.iskel"));
        if(modelKey!=mesh+skel) {
            if(!SceneInstanceLoader::LoadPreviewMeshes(mesh,skel,*skeleton,meshes))throw std::runtime_error("Preview model could not be decoded");
            modelKey=mesh+skel;clipPath.clear();
            global.resize(skeleton->boneCount);skin.resize(skeleton->boneCount);
        }
        if(clipPath!=d.clip) {
            std::vector<uint8_t> bytes;Resource::AssetFS::Get().ReadFile(d.clip,bytes);
            auto loaded=Resource::AnimationLoader{}.Load(d.clip,bytes);
            auto* r=dynamic_cast<Resource::AnimationResource*>(loaded.resource.get());
            if(!r||r->clips.empty())throw std::runtime_error("Preview animation missing");
            clip=r->clips[0].BindToSkeleton(*skeleton);clipPath=d.clip;
        }
        return true;
    }
    void Pose(float seconds) {
        SamplePreviewPose(*skeleton,clip,seconds,global,skin,rotations);
    }
};
struct State {
    bool open=false,playing=false,loop=true,compare=false,dirty=false,editing=false,chain=false;
    std::string path,pending,error,notice;
    Json doc,saved,set;
    std::vector<Json> undo,redo;
    std::shared_ptr<const Attack::Definition> definition;
    std::unique_ptr<Preview> preview;
    char pathInput[1024]{},setInput[1024]="asset/RossiDuel/AttackSets/Enemy.attackset.json";
    float elapsed=0,yaw=.65f,pitch=.15f,zoom=4.6f,scale=.1f;
    int selectedKey=0,dragKey=-1;
    TimingTrackDrag timingDrag;
};
State s;
void Refresh() {
    try {s.definition=Attack::Parse(s.doc,s.path);s.error.clear();}
    catch(const std::exception& e){s.error=e.what();}
    s.dirty=s.doc!=s.saved;
}
void Change(const Json& before) {
    if(before==s.doc)return;
    if(!s.editing){s.undo.push_back(before);if(s.undo.size()>64)s.undo.erase(s.undo.begin());s.redo.clear();}
    Refresh();
}
void LoadNow(const std::string& path) {
    std::string source;if(!Resource::AssetFS::Get().ReadFileText(path,source))throw std::runtime_error("Attack file not found");
    auto d=Attack::Parse(Json::parse(source),path);s.path=path;s.doc=d->document;s.saved=s.doc;s.definition=d;
        if(!s.doc.contains("distance"))s.doc["distance"]=d->distance;
    if(!s.doc.contains("blendOut"))s.doc["blendOut"]=d->blendOut;
    s.saved=s.doc;
    s.undo.clear();s.redo.clear();s.error.clear();s.dirty=false;s.elapsed=0;
    strncpy_s(s.pathInput,path.c_str(),_TRUNCATE);
}
void Undo(bool redo) {
    auto& from=redo?s.redo:s.undo;auto& to=redo?s.undo:s.redo;
    if(from.empty())return;to.push_back(s.doc);s.doc=from.back();from.pop_back();Refresh();
}
void Save(bool as) {
    std::string dst=as?s.pathInput:s.path;
    if(!dst.ends_with(".attack.json"))throw std::runtime_error("Save path must end with .attack.json");
    Attack::Save(dst,s.doc);s.path=dst;s.saved=s.doc;s.dirty=false;s.notice="Saved. Apply to use this version on the next attack.";
}
void Field(const char* name,const char* key,float min,float max) {
    float v=s.doc.value(key,0.f);
    if(EditorUI::DragFloat(name,&v,min,max,"%.3f"))s.doc[key]=v;
}
void Curve() {
    ImGui::TextUnformatted("Playback speed   X: animation progress   Y: multiplier");
    auto& keys=s.doc["speedCurve"];
    const ImVec2 origin=ImGui::GetCursorScreenPos(),size(ImGui::GetContentRegionAvail().x,145);
    auto* dl=ImGui::GetWindowDrawList();dl->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(18,24,32,255),4);
    auto xy=[&](float u,float v){return ImVec2(origin.x+u*size.x,origin.y+size.y-v/4.f*size.y);};
    for(int i=1;i<4;++i)dl->AddLine(xy(0,float(i)),xy(1,float(i)),IM_COL32(50,60,70,255));
    if(s.definition)for(int i=1;i<=200;++i)dl->AddLine(xy((i-1)/200.f,s.definition->speed.Sample((i-1)/200.f)),xy(i/200.f,s.definition->speed.Sample(i/200.f)),IM_COL32(88,201,216,255),2);
    ImGui::InvisibleButton("##curve",size);
    if(ImGui::IsItemHovered()&&ImGui::IsMouseClicked(0)) {
        float best=100;
        for(int i=0;i<int(keys.size());++i){auto p=xy(keys[i]["u"],keys[i]["value"]);auto m=ImGui::GetIO().MousePos;float d=(p.x-m.x)*(p.x-m.x)+(p.y-m.y)*(p.y-m.y);if(d<best){best=d;s.dragKey=s.selectedKey=i;}}
    }
    if(s.dragKey>=0&&ImGui::IsMouseDown(0)) {
        auto m=ImGui::GetIO().MousePos;auto& key=keys[s.dragKey];
        key["value"]=std::clamp((origin.y+size.y-m.y)/size.y*4.f,.05f,4.f);
        if(s.dragKey>0&&s.dragKey<int(keys.size())-1)
            key["u"]=std::clamp((m.x-origin.x)/size.x,keys[s.dragKey-1]["u"].get<float>()+.001f,keys[s.dragKey+1]["u"].get<float>()-.001f);
    } else s.dragKey=-1;
    for(int i=0;i<int(keys.size());++i)dl->AddCircleFilled(xy(keys[i]["u"],keys[i]["value"]),i==s.selectedKey?5.f:3.f,IM_COL32(240,195,80,255));
    s.selectedKey=std::clamp(s.selectedKey,0,int(keys.size())-1);
    ImGui::SetNextItemWidth(90);ImGui::InputInt("Key",&s.selectedKey);s.selectedKey=std::clamp(s.selectedKey,0,int(keys.size())-1);
    ImGui::SameLine();if(ImGui::Button("Add key")&&keys.size()<128) {
        int i=std::min(s.selectedKey,int(keys.size())-2);Json key={{"u",(keys[i]["u"].get<float>()+keys[i+1]["u"].get<float>())*.5f},{"value",1},{"interp","linear"}};
        keys.insert(keys.begin()+i+1,key);s.selectedKey=i+1;
    }
    ImGui::SameLine();if(ImGui::Button("Remove key")&&s.selectedKey>0&&s.selectedKey<int(keys.size())-1)keys.erase(keys.begin()+s.selectedKey--);
    auto& key=keys[s.selectedKey];float v=key["value"],u=key["u"];
    if(EditorUI::DragFloat("Multiplier",&v,.05f,4))key["value"]=v;
    if(s.selectedKey>0&&s.selectedKey<int(keys.size())-1&&EditorUI::DragFloat("Progress",&u,keys[s.selectedKey-1]["u"].get<float>()+.001f,keys[s.selectedKey+1]["u"].get<float>()-.001f))key["u"]=u;
    std::string interp=key.value("interp",std::string("linear"));int kind=interp=="step"?1:interp=="hermite"?2:0;
    if(ImGui::Combo("Interpolation",&kind,"Linear\0Step\0Hermite (bounded)\0"))key["interp"]=kind==0?"linear":kind==1?"step":"hermite";
    if(kind==2){float in=key.value("in",0.f),out=key.value("out",0.f);if(EditorUI::DragFloat("Tangent in",&in,-20,20))key["in"]=in;if(EditorUI::DragFloat("Tangent out",&out,-20,20))key["out"]=out;}
}
void View(ImVec2 origin,ImVec2 size,bool original) {
    auto& p=*s.preview;auto& d=*s.definition;float u=original?std::min(d.finish,s.elapsed/d.clipSeconds):d.ProgressAt(s.elapsed,d.clipSeconds);
    p.Pose(u*d.clipSeconds);float travel=d.Displacement(u);
    auto* dl=ImGui::GetWindowDrawList();dl->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(27,33,43,255),4);
    dl->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    XMVECTOR focus=XMVectorSet(0,1,travel*.4f,1);
    XMVECTOR eye=XMVectorAdd(focus,XMVectorSet(std::sin(s.yaw)*s.zoom,std::sin(s.pitch)*s.zoom+1,-std::cos(s.yaw)*s.zoom,0));
    XMMATRIX vp=XMMatrixLookAtLH(eye,focus,XMVectorSet(0,1,0,0))*XMMatrixPerspectiveFovLH(.8f,size.x/size.y,.05f,100);
    auto project=[&](XMVECTOR v){XMFLOAT4 a;XMStoreFloat4(&a,XMVector4Transform(XMVectorSetW(v,1),vp));return XMFLOAT4(origin.x+(a.x/a.w*.5f+.5f)*size.x,origin.y+(.5f-a.y/a.w*.5f)*size.y,a.z/a.w,a.w);};
    auto line=[&](XMVECTOR a,XMVECTOR b,ImU32 color){auto x=project(a),y=project(b);if(x.w>.05f&&y.w>.05f)dl->AddLine(ImVec2(x.x,x.y),ImVec2(y.x,y.y),color,1);};
    for(int i=-5;i<=5;++i){line(XMVectorSet(float(i),0,-5,1),XMVectorSet(float(i),0,8,1),IM_COL32(57,66,74,255));line(XMVectorSet(-5,0,float(i),1),XMVectorSet(5,0,float(i),1),IM_COL32(57,66,74,255));}
    p.raster.Draw(p.meshes,p.skin,origin,size,vp,s.scale,{0,0,travel});
    bool hit=false;for(const auto& w:d.windows)hit|=u>=w.start&&u<w.end;
    const ImU32 color=hit?IM_COL32(255,90,65,255):IM_COL32(100,130,150,140);
    if(!d.weaponCollider) {
    const float h=d.halfHeight+d.radius;
    XMVECTOR corners[8];for(int i=0;i<8;++i)corners[i]=XMVectorSet(d.offsetX+(i&1?d.radius:-d.radius),d.offsetY+(i&2?h:-h),travel+d.offsetZ+(i&4?d.radius:-d.radius),1);
    for(int i=0;i<8;++i)for(int bit:{1,2,4})if(!(i&bit))line(corners[i],corners[i|bit],color);
    }
    line(XMVectorSet(0,.03f,0,1),XMVectorSet(0,.03f,d.distance,1),IM_COL32(255,190,75,255));
    // Target dummy and facing reference are visual only.
    line(XMVectorSet(0,0,2,1),XMVectorSet(0,1.8f,2,1),IM_COL32(95,206,160,255));
    line(XMVectorSet(-.4f,1.3f,2,1),XMVectorSet(.4f,1.3f,2,1),IM_COL32(95,206,160,255));
    dl->AddText(ImVec2(origin.x+10,origin.y+10),IM_COL32(235,235,235,255),original?"Original timing":"Attack timing");
    dl->PopClipRect();
}
}
void Open(const std::string& path) {
    s.open=true;
    if(s.dirty){s.pending=path;return;}
    try {if(path.ends_with(".attackset.json")){s.set=Attack::LoadSet(path);strncpy_s(s.setInput,path.c_str(),_TRUNCATE);LoadNow(s.set["attacks"].begin().value());}else LoadNow(path);}
    catch(const std::exception& e){s.error=e.what();}
}
void Draw() {
    if(!s.open)return;
    ImGui::SetNextWindowSize(ImVec2(1180,850),ImGuiCond_FirstUseEver);
    bool open=true;
    if(!ImGui::Begin("Attack Editor",&open)){ImGui::End();return;}
    if(!open){if(s.dirty)s.pending="##close";else{s.open=false;s.preview.reset();ImGui::End();return;}}
    int childDepth=0,popupDepth=0;
    try {
        if(s.pending.size())ImGui::OpenPopup("Unsaved attack");
        if(ImGui::BeginPopupModal("Unsaved attack",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ++popupDepth;
            ImGui::TextUnformatted("Save changes before leaving this attack?");
            bool leave=false;
            if(ImGui::Button("Save and continue")){Save(false);leave=true;}ImGui::SameLine();
            if(ImGui::Button("Discard draft"))leave=true;ImGui::SameLine();
            if(ImGui::Button("Keep editing")){s.pending.clear();ImGui::CloseCurrentPopup();}
            if(leave){auto next=s.pending;s.pending.clear();s.dirty=false;ImGui::CloseCurrentPopup();if(next=="##close"){s.open=false;s.preview.reset();}else Open(next);}
            ImGui::EndPopup();--popupDepth;
        }
        if(!s.open){ImGui::End();return;}
        ImGui::SetNextItemWidth(600);ImGui::InputText("Asset / Save As",s.pathInput,sizeof(s.pathInput));ImGui::SameLine();
        if(ImGui::Button("Open"))Open(s.pathInput);
        if(ImGui::Button("Save"))Save(false);ImGui::SameLine();if(ImGui::Button("Save As"))Save(true);ImGui::SameLine();
        if(ImGui::Button("Reload"))Open(s.path);ImGui::SameLine();
        if(ImGui::Button("Apply next attack")){Attack::Apply(s.path,s.doc);s.notice="Applied. Active attacks keep their current version.";}
        ImGui::SameLine();if(ImGui::Button("Undo"))Undo(false);ImGui::SameLine();if(ImGui::Button("Redo"))Undo(true);
        ImGui::SameLine();ImGui::TextUnformatted(s.dirty?"Modified":"Saved");
        ImGui::InputText("Attack Set",s.setInput,sizeof(s.setInput));ImGui::SameLine();if(ImGui::Button("Load set"))s.set=Attack::LoadSet(s.setInput);
        if(s.set.contains("attacks")) {
            if(ImGui::BeginCombo("Attack",s.path.c_str())){for(auto it=s.set["attacks"].begin();it!=s.set["attacks"].end();++it)if(ImGui::Selectable(it.key().c_str(),s.path==it.value().get<std::string>()))Open(it.value());ImGui::EndCombo();}
        }
        if(!s.notice.empty())ImGui::TextWrapped("%s",s.notice.c_str());
        if(s.doc.is_null()){ImGui::TextWrapped("%s",s.error.c_str());ImGui::End();return;}
        Json before=s.doc;
        ImGui::BeginChild("Properties",ImVec2(330,0),true);
        ++childDepth;
        for(const char* key:{"id","name","clip","alias","previewMesh","previewSkeleton"}) {
            char value[1024]{};strncpy_s(value,s.doc.value(key,std::string{}).c_str(),_TRUNCATE);
            if(ImGui::InputText(key,value,sizeof(value)))s.doc[key]=value;
            if(std::string(key)=="clip"&&ImGui::BeginDragDropTarget()) {
                if(auto* payload=ImGui::AcceptDragDropPayload("IANIM_PATH"))s.doc["clip"]=std::string(static_cast<const char*>(payload->Data));
                ImGui::EndDragDropTarget();
            }
        }
        Field("Base multiplier","baseSpeed",.05f,4);Field("Windup ends","windup",0,1);Field("Recovery starts","recovery",0,1);Field("Finish","finish",.01f,1);
        static float s_rates[3]={.65f,1.2f,.9f};static bool s_smooth=true;
        ImGui::SeparatorText("Segment preset");
        EditorUI::DragFloat("Windup rate",&s_rates[0],.05f,4);EditorUI::DragFloat("Strike rate",&s_rates[1],.05f,4);EditorUI::DragFloat("Recovery rate",&s_rates[2],.05f,4);ImGui::Checkbox("Ease transitions",&s_smooth);
        if(ImGui::Button("Replace curve with segments"))ImGui::OpenPopup("Replace curve?");
        if(ImGui::BeginPopup("Replace curve?")){
            ++popupDepth;
            ImGui::TextUnformatted("Replaces custom points. Undo restores the original.");
            if(ImGui::Button("Use segments")){
                float w=s.doc["windup"],r=s.doc["recovery"];
                Json keys=Json::array();
                keys.push_back({{"u",0},{"value",s_rates[0]},{"interp",s_smooth?"hermite":"step"}});
                if(s_smooth&&w>.02f)keys.push_back({{"u",w-.02f},{"value",s_rates[0]},{"interp","hermite"}});
                keys.push_back({{"u",w},{"value",s_rates[1]},{"interp",s_smooth?"hermite":"step"}});
                if(s_smooth&&r>w+.02f)keys.push_back({{"u",r-.02f},{"value",s_rates[1]},{"interp","hermite"}});
                keys.push_back({{"u",r},{"value",s_rates[2]},{"interp","linear"}});keys.push_back({{"u",1},{"value",s_rates[2]},{"interp","linear"}});
                s.doc["speedCurve"]=keys;ImGui::CloseCurrentPopup();
            }ImGui::EndPopup();--popupDepth;
        }
        ImGui::SeparatorText("Combat");Field("Stamina cost","cost",0,1000);Field("Damage","damage",0,10000);Field("Guard stamina damage","staminaDamage",0,1000);Field("Blend in (seconds)","blendIn",0,1);
        bool cancel=!s.doc.at("cancel").empty();
        Field("Blend out (seconds)","blendOut",0,2);
        if(ImGui::Checkbox("Allow dodge cancel",&cancel))s.doc["cancel"]=cancel?Json::array({"dodge"}):Json::array();
        auto& hit=s.doc["hitbox"];
        bool weapon=hit.value("source",std::string("weapon"))=="weapon";
        if(ImGui::Checkbox("Use attached weapon collider",&weapon))hit["source"]=weapon?"weapon":"owner";
        if(weapon) {
            ImGui::TextWrapped("Attach a Box, Sphere or Capsule collider to the weapon and set its layer to AttackBox. Hit windows below enable damage. No rigid body is needed.");
            char tag[128]{};strncpy_s(tag,hit.value("weaponTag",std::string{}).c_str(),_TRUNCATE);
            if(ImGui::InputText("Weapon collider tag (optional)",tag,sizeof(tag)))hit["weaponTag"]=tag;
            ImGui::TextWrapped("Edit the shape on the weapon in the scene Inspector; this preview shows animation timing only.");
        } else for(const char* key:{"radius","halfHeight","x","y","z"}){float v=hit.value(key,0.f);if(EditorUI::DragFloat(key,&v,key[0]=='r'?.01f:key[0]=='h'?0.f:-10.f,10))hit[key]=v;}
        auto& windows=s.doc["hitWindows"];int remove=-1;
        for(int i=0;i<int(windows.size());++i){ImGui::PushID(i);float a=windows[i]["start"],b=windows[i]["end"];if(EditorUI::DragFloat("Hit start",&a,0,1))windows[i]["start"]=a;if(EditorUI::DragFloat("Hit end",&b,0,1))windows[i]["end"]=b;if(windows.size()>1&&ImGui::Button("Remove window"))remove=i;ImGui::PopID();}
        if(remove>=0)windows.erase(windows.begin()+remove);
        if(windows.size()<4&&ImGui::Button("Add hit window")){float start=windows.back()["end"];windows.push_back({{"start",start+.01f},{"end",std::min(.91f,start+.1f)}});}
        ImGui::SeparatorText("Motion / facing / combo");
        Field("Distance (metres)","distance",0,15);Field("Motion ends","moveEnd",.001f,1);Field("Track target until","turnEnd",0,1);Field("Turn radians / sec","turnRate",0,30);
        Field("Combo starts","comboStart",0,1);Field("Combo ends","comboEnd",0,1);
        char next[128]{};strncpy_s(next,s.doc.value("next",std::string{}).c_str(),_TRUNCATE);if(ImGui::InputText("Next attack key",next,sizeof(next)))s.doc["next"]=next;
        if(ImGui::TreeNode("Motion distance curve")) {auto& motion=s.doc["motionCurve"];for(int i=1;i<int(motion.size())-1;++i){ImGui::PushID(i);float u=motion[i]["u"],v=motion[i]["value"];if(EditorUI::DragFloat("Progress",&u,.001f,.999f))motion[i]["u"]=u;if(EditorUI::DragFloat("Distance fraction",&v,0,1))motion[i]["value"]=v;ImGui::PopID();}if(motion.size()==2&&ImGui::Button("Add midpoint"))motion.insert(motion.begin()+1,{{"u",.5f},{"value",.5f},{"interp","linear"}});ImGui::TreePop();}
        ImGui::EndChild();--childDepth;ImGui::SameLine();ImGui::BeginChild("PreviewAndCurve");++childDepth;
        if(ImGui::Button(s.playing?"Pause":"Play"))s.playing=!s.playing;ImGui::SameLine();if(ImGui::Button("Restart"))s.elapsed=0;ImGui::SameLine();if(ImGui::Button("Step 1/60")){s.playing=false;s.elapsed+=1/60.f;}
        ImGui::SameLine();ImGui::Checkbox("Loop",&s.loop);ImGui::SameLine();ImGui::Checkbox("A/B",&s.compare);ImGui::SameLine();ImGui::Checkbox("Combo preview",&s.chain);
        const bool propertiesChanged=before!=s.doc;
        Change(before);before=s.doc;
        if(s.definition) {
            float duration=float(s.definition->Duration(s.definition->clipSeconds));
            if(s.playing){s.elapsed+=std::min(ImGui::GetIO().DeltaTime,.1f);if(s.elapsed>duration){if(s.chain&&s.set.contains("attacks")&&s.set["attacks"].contains(s.definition->next)&&!s.dirty)LoadNow(s.set["attacks"][s.definition->next]);else if(s.loop)s.elapsed=0;else{s.elapsed=duration;s.playing=false;}}}
            if(EditorUI::DragFloat("Time (seconds)",&s.elapsed,0,duration))s.playing=false;
            ImGui::Text("Source %.3f  |  Windup %.3fs  |  Total %.3fs",s.definition->ProgressAt(s.elapsed,s.definition->clipSeconds),s.definition->TimeAt(s.definition->windup)*s.definition->clipSeconds,duration);
            EditorUI::DragFloat("Orbit",&s.yaw,-3.14f,3.14f);ImGui::SameLine();EditorUI::DragFloat("Zoom",&s.zoom,2,10);
            EditorUI::DragFloat("Model scale",&s.scale,.001f,10);
            if(!s.preview)s.preview=std::make_unique<Preview>();s.preview->Load(*s.definition);
            ImVec2 pos=ImGui::GetCursorScreenPos(),size(ImGui::GetContentRegionAvail().x,290);
            if(s.compare){ImVec2 half((size.x-6)/2,size.y);View(pos,half,true);View(ImVec2(pos.x+half.x+6,pos.y),half,false);}else View(pos,size,false);
            ImGui::Dummy(size);
            float scrubProgress=-1;
            if(DrawTimingTracks(*s.definition,s.doc,s.timingDrag,
                s.definition->ProgressAt(s.elapsed,s.definition->clipSeconds),scrubProgress))s.playing=false;
            if(scrubProgress>=0)s.elapsed=float(s.definition->TimeAt(scrubProgress)*s.definition->clipSeconds);
        }
        Curve();Change(before);s.editing=ImGui::IsAnyItemActive()&&(s.editing||propertiesChanged||before!=s.doc);
        if(!s.error.empty())ImGui::TextColored(ImVec4(1,.3f,.25f,1),"%s",s.error.c_str());
        ImGui::EndChild();--childDepth;
    }catch(const std::exception& e){while(popupDepth-->0)ImGui::EndPopup();while(childDepth-->0)ImGui::EndChild();s.error=e.what();ImGui::TextWrapped("%s",e.what());}
    ImGui::End();
}
std::string Control(const std::string& request) {
    try {
        auto cmd=Json::parse(request);std::string action=cmd.value("action",std::string("state"));
        if(action=="open"){if(s.dirty)throw std::runtime_error("Unsaved attack draft");Open(cmd.at("path"));}
        else if(action=="replace"){Json before=s.doc;s.doc=cmd.at("document");s.editing=false;Change(before);}
        else if(action=="undo")Undo(false);
        else if(action=="redo")Undo(true);
        else if(action=="save")Save(false);
        else if(action=="saveAs"){strncpy_s(s.pathInput,cmd.at("path").get<std::string>().c_str(),_TRUNCATE);Save(true);}
        else if(action=="apply")Attack::Apply(s.path,s.doc);
        else if(action=="reload"){if(s.dirty)throw std::runtime_error("Unsaved attack draft");LoadNow(s.path);}
        else if(action=="seek"){s.elapsed=cmd.at("seconds");s.playing=false;}
        else if(action=="play"){s.elapsed=0;s.playing=true;}
        else if(action=="close"){if(s.dirty)throw std::runtime_error("Unsaved attack draft");s.open=false;s.preview.reset();}
        else if(action!="state")throw std::runtime_error("Unknown attack editor command");
        return Json{{"ok",true},{"open",s.open},{"path",s.path},{"dirty",s.dirty},{"error",s.error},
            {"document",s.doc},{"previewMeshes",s.preview?s.preview->meshes.size():0},
            {"previewTriangles",s.preview?s.preview->raster.triangles.size():0},{"elapsed",s.elapsed}}.dump();
    }catch(const std::exception& e){return Json{{"ok",false},{"error",e.what()}}.dump();}
}
}
