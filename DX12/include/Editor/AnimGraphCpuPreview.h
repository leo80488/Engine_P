#pragma once
#include "Editor/PreviewSkin.h"
#include "Editor/CpuSkinRaster.h"
#include "Anim/Graph/AnimGraphInstance.h"
#include "Resource/AnimationLoader.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimSetLoader.h"
#include "Resource/AssetFS.h"
#include "Resource/HumanoidProfileBuilder.h"
#include <map>

namespace Editor {
class AnimGraphCpuPreview {
public:
    void Load(std::shared_ptr<const Anim::Graph::Definition> definition,
        const std::string& mesh,const std::string& skel,const std::string& setPath) {
        if(modelKey!=mesh+"|"+skel) {
            if(!SceneInstanceLoader::LoadPreviewMeshes(mesh,skel,*skeleton,meshes))throw std::runtime_error("CPU preview model could not be decoded");
            if(!skeleton->humanoid.IsValid())Resource::HumanoidProfileBuilder::Build(*skeleton);
            modelKey=mesh+"|"+skel;
        }
        instance=std::make_unique<Anim::Graph::Instance>(definition,Anim::Graph::BoundGraph::Bind(*definition,*skeleton));
        clips=ClipLibrary{};renders=triangles=0;
        std::map<std::string,Resource::AnimSetResource::Clip> aliases;
        if(!setPath.empty()) {
            std::vector<uint8_t> bytes;if(!Resource::AssetFS::Get().ReadFile(setPath,bytes))throw std::runtime_error("CPU preview Animation Set not found");
            auto loaded=Resource::AnimSetLoader{}.Load(setPath,bytes);auto* set=dynamic_cast<Resource::AnimSetResource*>(loaded.resource.get());
            if(!set || !set->error.empty())throw std::runtime_error(set?set->error:"Invalid CPU Animation Set");aliases=set->slots;
        }
        for(const auto& slot:definition->slots)if(!aliases.contains(slot)) {
            if(!slot.ends_with(".ianim"))throw std::runtime_error("CPU preview needs an Animation Set for alias: "+slot);
            aliases[slot]={slot,"",true,1};
        }
        std::map<std::string,std::unique_ptr<Resource::Resource>> sources;
        for(const auto& [alias,reference]:aliases) {
            auto& source=sources[reference.path];
            if(!source){std::vector<uint8_t> bytes;if(!Resource::AssetFS::Get().ReadFile(reference.path,bytes))throw std::runtime_error("CPU animation not found: "+reference.path);source=Resource::AnimationLoader{}.Load(reference.path,bytes).resource;}
            auto* animation=dynamic_cast<Resource::AnimationResource*>(source.get());
            const int selected=animation?Resource::FindSourceClip(animation->clips,reference.sourceClip):-1;
            if(selected<0)throw std::runtime_error("CPU source clip not found: "+reference.path+" / "+reference.sourceClip);
            instance->slots[alias]=clips.Register(animation->clips[selected].BindToSkeleton(*skeleton));
            instance->aliasSettings[alias]={reference.looping,reference.rate};
        }
        pose.resize(skeleton->boneCount);instance->Evaluate(clips,0,pose.data());
    }
    void Tick(float dt) {
        if(!instance)return;
        instance->Evaluate(clips,dt,pose.data());
    }
    void Draw(ImVec2 size) {
        using namespace DirectX;
        if(!instance || instance->lastOutput.empty())return;
        BuildPreviewSkin(*skeleton,instance->lastOutput,scratch,global,skin);
        const auto origin=ImGui::GetCursorScreenPos();auto* dl=ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(27,33,43,255),4);
        dl->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
        const auto focus=XMVectorSet(0,1,0,1);const auto eye=XMVectorAdd(focus,XMVectorSet(std::sin(yaw)*zoom,std::sin(pitch)*zoom+1,-std::cos(yaw)*zoom,0));
        const auto vp=XMMatrixLookAtLH(eye,focus,XMVectorSet(0,1,0,0))*XMMatrixPerspectiveFovLH(.8f,size.x/size.y,.05f,100);
        triangles=raster.Draw(meshes,skin,origin,size,vp,scale);++renders;
        dl->PopClipRect();ImGui::InvisibleButton("CPU graph pose",size);
        if(ImGui::IsItemHovered()){zoom=std::clamp(zoom-ImGui::GetIO().MouseWheel*.3f,.5f,30.f);if(ImGui::IsMouseDragging(0)){yaw+=ImGui::GetIO().MouseDelta.x*.01f;pitch=std::clamp(pitch+ImGui::GetIO().MouseDelta.y*.01f,-1.f,1.f);}}
    }

    std::unique_ptr<SkeletonAsset> skeleton=std::make_unique<SkeletonAsset>();
    std::vector<SceneInstanceLoader::PreviewMeshData> meshes;
    std::unique_ptr<Anim::Graph::Instance> instance;
    ClipLibrary clips;
    CpuSkinRaster raster;
    std::vector<Anim::LocalPose> pose,scratch;
    std::vector<DirectX::XMFLOAT4X4> global,skin;
    std::string modelKey;
    size_t renders=0,triangles=0;
    float scale=.1f,yaw=.65f,pitch=.15f,zoom=4.6f;
};
}
