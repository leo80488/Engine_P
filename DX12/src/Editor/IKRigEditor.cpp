#include "Editor/EditorLayer.h"
#include "ECS/IKRigAuthoring.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "Resource/SkeletonAsset.h"
#include "Anim/IK/Kinematics.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
using namespace DirectX;
namespace {
void BoneCombo(const char* label,std::string& name,const SkeletonAsset* asset,bool controls=false) {
    if(!ImGui::BeginCombo(label,name.empty()?"Auto (humanoid role)":name.c_str()))return;
    if(ImGui::Selectable("Auto",name.empty()))name.clear();
    if(asset) {
        if(controls && !asset->ikChains.empty()) {
            ImGui::TextDisabled("MMD controls");
            for(const auto& chain:asset->ikChains)if(chain.ikBoneIndex<asset->boneCount) {
                const auto* bone=asset->boneNames[chain.ikBoneIndex];
                if(ImGui::Selectable(bone,name==bone))name=bone;
            }
            ImGui::Separator();
        }
        for(uint32_t b=0;b<asset->boneCount;++b) {
            ImGui::PushID(int(b));
            if(ImGui::Selectable(asset->boneNames[b],name==asset->boneNames[b]))name=asset->boneNames[b];
            if(ImGui::IsItemHovered())for(uint32_t r=0;r<kHumanoidBoneCount;++r)
                if(asset->humanoid.boneForRole[r]==int32_t(b))ImGui::SetTooltip("Humanoid role: %s",HumanoidRoleName(HumanoidBone(r)));
            ImGui::PopID();
        }
    }
    ImGui::EndCombo();
}
template<class E> void EnumCombo(const char* label,E& value,const char* const* options,int count) {
    int index=int(value);if(ImGui::Combo(label,&index,options,count))value=E(index);
}
}
void EditorLayer::DrawIKRigInspector(void* ptr,World* world,Entity entity) {
    auto& rig=*static_cast<IKRigComponent*>(ptr);
    const auto* skeleton=world?world->GetComponent<SkeletonComponent>(entity):nullptr;
    const auto* asset=skeleton && m_renderer && skeleton->assetIndex<m_renderer->GetSkeletonRegistry().Count()
        ? &m_renderer->GetSkeletonRegistry().Get(skeleton->assetIndex):nullptr;
    ImGui::Checkbox("Enabled",&rig.enabled);ImGui::SliderFloat("Master weight",&rig.masterWeight,0,1);
    ImGui::Checkbox("Debug overlay",&rig.debugDraw);
    ImGui::DragFloat("Max distance (0 = unlimited)",&rig.maxDistance,.5f,0,1000);
    int interval=int(rig.probeEveryFrames);if(ImGui::SliderInt("Probe interval (frames)",&interval,1,16))rig.probeEveryFrames=uint32_t(interval);
    if(rig.debugDraw && m_renderer && m_renderer->GetDebugWirePass())m_renderer->GetDebugWirePass()->enabled=true;
    if(ImGui::Button("Auto-detect"))rig.resolveKey=0;
    if(ImGui::CollapsingHeader("Grounding")) {
        VisitIKRigAuthoring(rig,[](const char* key,auto& value,const auto&) {
            if(std::string_view(key).substr(0,7)!="ground.")return;
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,bool>)ImGui::Checkbox(key+7,&value);
            else if constexpr(std::is_same_v<T,float>)ImGui::DragFloat(key+7,&value,.01f);
            else if constexpr(std::is_same_v<T,uint32_t>) {int mask=int(value);if(ImGui::InputInt("Ground layers",&mask))value=uint32_t(mask)&0xffff;}
        });
        ImGui::TextDisabled("Distances use model units. Sole offset < 0 uses rest ankle height.");
        ImGui::Text("Pelvis %.3f, grounded %s",rig.pelvisOffset,rig.grounded?"yes":"no");
    }
    const char* groups[]={"Feet","Hands","Custom"};const size_t starts[]={0,2,4},ends[]={2,4,8};
    static const char* kRefs[]={"Auto","IK bone","Virtual"};
    static const char* kSolvers[]={"Auto","MMD chain","Two bone","CCD","FABRIK","LookAt"};
    static const char* kPoles[]={"Animated","Character forward","Bone","World point"};
    static const char* kRotations[]={"Keep animated","Match target"};
    for(size_t group=0;group<3;++group)if(ImGui::CollapsingHeader(groups[group]))for(size_t i=starts[group];i<ends[group];++i) {
        ImGui::PushID(int(i));
        if(ImGui::TreeNode(kIKGoalNames[i])) {
            auto& def=rig.goals[i];const auto& rt=rig.rt[i];
            ImGui::Checkbox("Enabled",&def.enabled);ImGui::SliderFloat("Weight",&def.weight,0,1);
            if(i>=size_t(IKGoalSlot::Custom0)) {
                ImGui::Checkbox("Curve-driven target",&def.curveDriven);
                if(def.curveDriven) {
                    char curve[256];snprintf(curve,sizeof(curve),"%s",def.curve.c_str());
                    if(ImGui::InputText("Graph curve",curve,sizeof(curve)))def.curve=curve;
                    ImGui::DragFloat3("Target model position",&def.curveTarget.x,.01f);
                }
            }
            EnumCombo("Reference",def.refMode,kRefs,3);EnumCombo("Solver",def.solver,kSolvers,6);
            BoneCombo("IK bone",def.ikBone,asset,true);BoneCombo("Root",def.rootBone,asset);
            BoneCombo("Mid",def.midBone,asset);BoneCombo("End",def.endBone,asset);
            EnumCombo("Pole",def.poleMode,kPoles,4);
            if(def.poleMode==IKPoleMode::Bone)BoneCombo("Pole bone",def.poleBone,asset);
            if(def.poleMode==IKPoleMode::WorldPoint)ImGui::DragFloat3(def.curveDriven?"Pole model point":"Pole world point",&def.polePoint.x,.01f);
            ImGui::SliderFloat("Softness",&def.softness,0,.5f);
            EnumCombo("End rotation",def.handRot,kRotations,2);
            if(def.solver==IKSolverKind::LookAt) {ImGui::DragFloat3("Look axis",&def.lookAxis.x,.01f);ImGui::SliderFloat("Look limit",&def.lookLimitDeg,0,180);}
            ImGui::Text("%s | error %.4fm | %s",IKPathName(rt.path),rt.error,rt.reached?"reached":"unreached");
            ImGui::Text("Weight %.2f | %s | delta %.3f",rt.weight,rt.locked?"locked":"free",rt.groundDelta);
            if(i>=2 && GetPlayState()==ViewportPlayState::Stopped) {
                if(ImGui::Button("Preview target gizmo")) {
                    m_ikPreviewSlot=int(i);m_ikPreviewEntity=entity;
                    auto& target=rig.script[i];target.space=IKTargetSpace::World;target.position=rt.effector;
                    const auto* poses=asset?m_renderer->GetAnimationSystem()->GetLocalPose(entity):nullptr;
                    const auto end=rig.resolved[i].end;
                    if(poses && end>=0 && uint32_t(end)<asset->boneCount) {
                        const auto* transform=world->GetComponent<GlobalTransform>(entity);
                        const auto frame=Anim::IK::BoneModel(end,poses,*asset)*(transform?XMLoadFloat4x4(&transform->matrix):XMMatrixIdentity());
                        XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,frame);
                        XMStoreFloat3(&target.position,t);XMStoreFloat4(&target.rotation,r);
                    }
                    target.active=true;target.weight=1;target.blend=0;
                }
                if(m_ikPreviewEntity==entity && m_ikPreviewSlot==int(i)) {
                    ImGui::DragFloat3("Target world position",&rig.script[i].position.x,.01f);
                    if(ImGui::Button("End preview")) {rig.script[i].active=false;m_ikPreviewSlot=-1;}
                }
                if(i==2 && ImGui::Button("Two-hand grip preset")) {
                    m_ikPreviewSlot=-1;
                    auto& target=rig.script[i];target.space=IKTargetSpace::SelfBone;target.socket="@RightHand";
                    target.position={-.2f,0,.2f};target.active=true;target.weight=1;target.blend=0;
                    def.handRot=IKHandRot::MatchTarget;
                }
                if(rig.script[i].space==IKTargetSpace::SelfBone)ImGui::DragFloat3("Grip local offset",&rig.script[i].position.x,.01f);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
void EditorLayer::RenderIKTargetGizmo(ImVec2 vpMin,ImVec2 vpMax) {
    if(GetPlayState()!=ViewportPlayState::Stopped || !m_world || m_selectedEntity!=m_ikPreviewEntity || m_ikPreviewSlot<0)return;
    auto* rig=m_world->GetComponent<IKRigComponent>(m_selectedEntity);if(!rig)return;
    auto& target=rig->script[size_t(m_ikPreviewSlot)];
    if(!target.active || target.space!=IKTargetSpace::World)return;
    if(!ImGuizmo::IsUsing())XMStoreFloat4x4(&m_ikGizmoMatrix,XMMatrixRotationQuaternion(XMLoadFloat4(&target.rotation))*XMMatrixTranslationFromVector(XMLoadFloat3(&target.position)));
    const auto position=XMLoadFloat4x4(&m_ikGizmoMatrix).r[3];
    auto clip=XMVector4Transform(position,XMLoadFloat4x4(&m_renderView.viewMatrix)*XMLoadFloat4x4(&m_renderView.projMatrixNoJitter));
    if(XMVectorGetW(clip)<=0)return;
    ImGuizmo::SetOrthographic(m_editorViewMode!=EditorViewMode::Perspective);
    ImGuizmo::SetRect(vpMin.x,vpMin.y,vpMax.x-vpMin.x,vpMax.y-vpMin.y);
    // Viewport's own clipped draw list keeps other dock windows above handles.
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
    ImGui::GetWindowDrawList()->PushClipRect(vpMin,vpMax,true);
    m_gizmoSubmittedThisFrame=true;
    const auto operation=m_gizmoOperation==ImGuizmo::ROTATE?ImGuizmo::ROTATE:ImGuizmo::TRANSLATE;
    bool changed=ImGuizmo::Manipulate(reinterpret_cast<const float*>(&m_renderView.viewMatrix),reinterpret_cast<const float*>(&m_renderView.projMatrixNoJitter),operation,ImGuizmo::WORLD,reinterpret_cast<float*>(&m_ikGizmoMatrix));
    ImGui::GetWindowDrawList()->PopClipRect();
    if(changed) {
        XMVECTOR s,r,t;XMMatrixDecompose(&s,&r,&t,XMLoadFloat4x4(&m_ikGizmoMatrix));
        XMStoreFloat3(&target.position,t);XMStoreFloat4(&target.rotation,r);target.hasRotation=operation==ImGuizmo::ROTATE || target.hasRotation;
    }
}
