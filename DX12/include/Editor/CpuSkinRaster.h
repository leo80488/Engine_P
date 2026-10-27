#pragma once
#include "Scene/SceneInstanceLoader.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <span>

namespace Editor {
struct CpuSkinRaster {
    struct Tri {ImVec2 a,b,c;float z;ImU32 color;};
    std::vector<Tri> triangles;
    std::vector<DirectX::XMFLOAT3> posed;
    std::vector<DirectX::XMFLOAT4> projected;
    size_t Draw(std::span<const SceneInstanceLoader::PreviewMeshData> meshes,
        std::span<const DirectX::XMFLOAT4X4> skin, ImVec2 origin,ImVec2 size,
        DirectX::FXMMATRIX vp,float scale,DirectX::XMFLOAT3 offset={}) {
        using namespace DirectX;auto& p=*this;auto* dl=ImGui::GetWindowDrawList();
        auto project=[&](XMVECTOR v){XMFLOAT4 a;XMStoreFloat4(&a,XMVector4Transform(XMVectorSetW(v,1),vp));return XMFLOAT4(origin.x+(a.x/a.w*.5f+.5f)*size.x,origin.y+(.5f-a.y/a.w*.5f)*size.y,a.z/a.w,a.w);};
    p.triangles.clear();
    for(size_t mi=0;mi<meshes.size();++mi) {
        const auto& mesh=meshes[mi];p.posed.resize(mesh.positions.size());p.projected.resize(mesh.positions.size());
        for(size_t i=0;i<mesh.positions.size();++i) {
            XMVECTOR v=XMLoadFloat3(&mesh.positions[i]),result=XMVectorZero();float total=0;
            for(int k=0;k<MAX_BONES_PER_VERTEX;++k)if(mesh.weights[i].boneWeights[k]) {
                auto bone=mesh.weights[i].boneIndices[k];if(bone>=skin.size())continue;
                float w=mesh.weights[i].boneWeights[k]/255.f;total+=w;
                result=XMVectorAdd(result,XMVectorScale(XMVector3TransformCoord(v,XMLoadFloat4x4(&skin[bone])),w));
            }
            if(total>0)result=XMVectorScale(result,1/total);else result=v;
            result=XMVectorAdd(XMVectorScale(result,scale),XMLoadFloat3(&offset));
            XMStoreFloat3(&p.posed[i],result);p.projected[i]=project(result);
        }
        for(size_t i=0;i+2<mesh.indices.size();i+=3) {
            auto ia=mesh.indices[i],ib=mesh.indices[i+1],ic=mesh.indices[i+2];
            if(ia>=p.posed.size()||ib>=p.posed.size()||ic>=p.posed.size())continue;
            const auto& a=p.projected[ia];const auto& b=p.projected[ib];const auto& c=p.projected[ic];if(a.w<=.05f||b.w<=.05f||c.w<=.05f)continue;
            auto n=XMVector3Normalize(XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&p.posed[ib]),XMLoadFloat3(&p.posed[ia])),XMVectorSubtract(XMLoadFloat3(&p.posed[ic]),XMLoadFloat3(&p.posed[ia]))));
            float shade=.35f+.65f*std::abs(XMVectorGetX(XMVector3Dot(n,XMVector3Normalize(XMVectorSet(.4f,.8f,-.5f,0)))));
            int r=int((mi%3==0?207:170)*shade),g=int((mi%3==0?152:180)*shade),bcol=int((mi%3==0?133:194)*shade);
            p.triangles.push_back({ImVec2(a.x,a.y),ImVec2(b.x,b.y),ImVec2(c.x,c.y),(a.z+b.z+c.z)/3,IM_COL32(r,g,bcol,255)});
        }
    }
    std::sort(p.triangles.begin(),p.triangles.end(),[](const auto& a,const auto& b){return a.z>b.z;});
    for(const auto& t:p.triangles)dl->AddTriangleFilled(t.a,t.b,t.c,t.color);
        return triangles.size();
    }
};
}
