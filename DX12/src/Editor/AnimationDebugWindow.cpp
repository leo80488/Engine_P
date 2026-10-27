#include "ECS/IKRigComponent.h"
// EditorLayer::RenderAnimationDebugWindow: the floating animation / skeleton state panel, in its
// own file to keep EditorLayer.cpp manageable.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include <string>
#include <cmath>
#include "ECS/AnimationSystem.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/SkeletonAsset.h"
#include "Renderer/Renderer.h"
#include "Graphics/SkinningBuffers.h"
#include "System/Log.h"
#include "imgui/imgui.h"

using namespace DirectX;

// ==== Bone matrix diagnostics ====

// True if any element is NaN or Inf.
static bool HasBadValues(const XMFLOAT4X4& m)
{
    const float* f = &m._11;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(f[i])) return true;
    return false;
}

// Determinant of the upper-left 3x3 (rotation + scale block).
static float Det3x3(const XMFLOAT4X4& m)
{
    return m._11 * (m._22 * m._33 - m._23 * m._32)
         - m._12 * (m._21 * m._33 - m._23 * m._31)
         + m._13 * (m._21 * m._32 - m._22 * m._31);
}

// Scale of the 3x3 block (length of each row).
static XMFLOAT3 ExtractScale(const XMFLOAT4X4& m)
{
    return {
        std::sqrtf(m._11*m._11 + m._12*m._12 + m._13*m._13),
        std::sqrtf(m._21*m._21 + m._22*m._22 + m._23*m._23),
        std::sqrtf(m._31*m._31 + m._32*m._32 + m._33*m._33)
    };
}

// True if every element is within @p tol of the identity.
static bool IsNearIdentity(const XMFLOAT4X4& m, float tol = 0.001f)
{
    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float* f = &m._11;
    for (int i = 0; i < 16; ++i)
        if (std::fabsf(f[i] - id[i]) > tol) return false;
    return true;
}

void EditorLayer::RenderAnimationDebugWindow()
{
    if (!m_showAnimDebug) return;
    if (!m_world || !m_renderer) return;

    ImGui::SetNextWindowSize(ImVec2(780, 840), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Debug", &m_showAnimDebug))
    {
        ImGui::End();
        return;
    }

    const Entity e = m_selectedEntity;
    if (e == NullEntity)
    {
        ImGui::TextDisabled("No entity selected. Pick a character entity in the Hierarchy.");
        ImGui::End();
        return;
    }

    const std::string& enameStr = m_world->GetName(e);
    const char* ename = enameStr.c_str();
    ImGui::Text("Entity #%u  \"%s\"", e, ename);
    ImGui::Separator();

    // Resolve the skeleton entity (the mesh child and the character root may differ).
    Entity skelEnt = e;
    if (const SkeletonRef* ref = m_world->GetComponent<SkeletonRef>(e))
        if (ref->entity != NullEntity) skelEnt = ref->entity;
    if (skelEnt != e)
        ImGui::TextDisabled("  (skeleton root = entity #%u)", skelEnt);

    const AnimationComponent* anim     = m_world->GetComponent<AnimationComponent>(skelEnt);
    const SkeletonComponent*  skelComp = m_world->GetComponent<SkeletonComponent>(skelEnt);
    const LocalTransform*     lt       = m_world->GetComponent<LocalTransform>(e);
    const GlobalTransform*    gt       = m_world->GetComponent<GlobalTransform>(e);

    const SkeletonAsset* skelAsset = nullptr;
    if (skelComp && skelComp->assetIndex != kInvalidAnimHandle)
    {
        const SkeletonRegistry& reg = m_renderer->GetSkeletonRegistry();
        if (skelComp->assetIndex < reg.Count())
            skelAsset = &reg.Get(skelComp->assetIndex);
    }

    AnimationSystem* animSys = m_animSys ? m_animSys : m_renderer->GetAnimationSystem();
    const AnimationSystem::LocalPose* localPoses =
        animSys ? animSys->GetLocalPose(skelEnt) : nullptr;

    // Skin matrices come from the PoseRingBuffer (CPU-mapped upload heap).
    const XMFLOAT4X4* skinMats = nullptr;
    if (skelComp && skelComp->poseByteOffset != ~0u)
        skinMats = m_renderer->GetPoseRingBuffer().ReadMapped(skelComp->poseByteOffset);

    // ---- AnimationComponent ----
    if (ImGui::CollapsingHeader("AnimationComponent", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!anim)
        {
            ImGui::TextDisabled("  (none on entity #%u)", skelEnt);
        }
        else
        {
            const ClipLibrary& clipLib = m_renderer->GetClipLibrary();
            ImGui::Text("  primaryClip : #%u", anim->primaryClip);
            if (anim->primaryClip != kInvalidAnimHandle && anim->primaryClip < clipLib.Count())
            {
                const ClipAsset& c = clipLib.Get(anim->primaryClip);
                ImGui::Text("    time   : %.4f / %.4f s", anim->primaryTime, c.duration);
                ImGui::Text("    frames : %u   rate: %.0f fps   bones(clip): %u   bones(skel): %u",
                            c.frameCount, c.frameRate, c.boneCount,
                            skelComp ? skelComp->boneCount : 0);
            }
            ImGui::Text("  speed  : %.2f   paused: %s   looping: %s",
                        anim->speed,
                        anim->paused  ? "YES" : "no",
                        anim->looping ? "YES" : "no");
        }
    }

    // ---- SkeletonComponent ----
    if (ImGui::CollapsingHeader("SkeletonComponent", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!skelComp)
        {
            ImGui::TextDisabled("  (none on entity #%u)", skelEnt);
        }
        else
        {
            ImGui::Text("  assetIndex : %u", skelComp->assetIndex);
            ImGui::Text("  boneCount  : %u", skelComp->boneCount);
            ImGui::Text("  poseOffset : 0x%08X%s",
                        skelComp->poseByteOffset,
                        skelComp->poseByteOffset == ~0u ? "  [INACTIVE]" : "");
        }
    }

    // ---- LocalTransform ----
    if (ImGui::CollapsingHeader("LocalTransform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!lt)
        {
            ImGui::TextDisabled("  (none on entity #%u)", e);
        }
        else
        {
            ImGui::Text("  pos : (%.5f, %.5f, %.5f)",
                        lt->translation.x, lt->translation.y, lt->translation.z);
            ImGui::Text("  rot : (%.5f, %.5f, %.5f, %.5f)",
                        lt->rotation.x,    lt->rotation.y,    lt->rotation.z, lt->rotation.w);
            ImGui::Text("  scl : (%.5f, %.5f, %.5f)",
                        lt->scale.x,       lt->scale.y,       lt->scale.z);
        }
    }

    // ---- GlobalTransform ----
    if (ImGui::CollapsingHeader("GlobalTransform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!gt)
        {
            ImGui::TextDisabled("  (none on entity #%u)", e);
        }
        else
        {
            const auto& m = gt->matrix;
            ImGui::Text("  worldPos : (%.5f, %.5f, %.5f)", m._41, m._42, m._43);
            if (ImGui::TreeNode("Full 4x4 Matrix (row-major)"))
            {
                ImGui::Text("  [%9.5f %9.5f %9.5f %9.5f]", m._11, m._12, m._13, m._14);
                ImGui::Text("  [%9.5f %9.5f %9.5f %9.5f]", m._21, m._22, m._23, m._24);
                ImGui::Text("  [%9.5f %9.5f %9.5f %9.5f]", m._31, m._32, m._33, m._34);
                ImGui::Text("  [%9.5f %9.5f %9.5f %9.5f]", m._41, m._42, m._43, m._44);
                ImGui::TreePop();
            }
        }
    }

    // ---- Bone local poses ----
    static char s_boneFilter[64] = {};
    if (ImGui::CollapsingHeader("Bone Local Poses (current frame)"))
    {
        if (!localPoses || !skelAsset)
        {
            ImGui::TextDisabled("  No active animation, or skeleton missing.\n"
                                "  Start playback and ensure the entity has SkeletonComponent.");
        }
        else
        {
            ImGui::InputText("Filter##boneflt", s_boneFilter, sizeof(s_boneFilter));
            const std::string filterStr(s_boneFilter);

            // Count grant bones.
            uint32_t grantCount = 0;
            for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
                if (skelAsset->grantSource[b] >= 0) ++grantCount;
            if (grantCount > 0)
                ImGui::Text("  Grant (D-bone) count: %u", grantCount);

            if (ImGui::BeginTable("BonePoseTable", 7,
                ImGuiTableFlags_Borders    | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_RowBg      | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0, 300)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#",           ImGuiTableColumnFlags_WidthFixed, 32);
                ImGui::TableSetupColumn("Par",         ImGuiTableColumnFlags_WidthFixed, 30);
                ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn("Grant",       ImGuiTableColumnFlags_WidthFixed, 35);
                ImGui::TableSetupColumn("Pos(x,y,z)",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Rot(x,y,z,w)",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Scl",         ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
                {
                    const char* bn = skelAsset->boneNames[b];
                    if (!filterStr.empty() &&
                        std::string(bn).find(filterStr) == std::string::npos)
                        continue;

                    const AnimationSystem::LocalPose& p = localPoses[b];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%u", b);
                    ImGui::TableSetColumnIndex(1);
                    {
                        int32_t par = skelAsset->parentIndex[b];
                        if (par < 0) ImGui::TextDisabled("root");
                        else         ImGui::Text("%d", par);
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(bn[0] ? bn : "(?)");
                    ImGui::TableSetColumnIndex(3);
                    {
                        int32_t gs = skelAsset->grantSource[b];
                        if (gs >= 0)
                            ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "%d", gs);
                    }
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.3f  %.3f  %.3f", p.pos.x, p.pos.y, p.pos.z);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.3f %.3f %.3f %.3f",
                                p.rot.x, p.rot.y, p.rot.z, p.rot.w);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%.3f %.3f %.3f", p.scl.x, p.scl.y, p.scl.z);
                }
                ImGui::EndTable();
            }
        }
    }

    // ---- Virtual bones ----
    // Each should sit on its `to` bone after every frame's evaluation; the table shows the model-space
    // distance between the two (bindPose x skin).
    if (skelAsset && !skelAsset->virtualBones.empty() && ImGui::CollapsingHeader("Virtual Bones"))
    {
        ImGui::TextDisabled("  %zu virtual bone(s) after %u real bone(s)", skelAsset->virtualBones.size(),
                            skelAsset->RealBoneCount());
        const bool live = skinMats && skelComp && skelComp->poseByteOffset != ~0u;
        if (ImGui::BeginTable("VirtualBoneTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                         ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("From -> To");
            ImGui::TableSetupColumn("|VB - To|", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();
            for (const SkeletonAsset::VirtualBoneDef& vb : skelAsset->virtualBones)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(skelAsset->boneNames[vb.index]);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s -> %s", skelAsset->boneNames[vb.from], skelAsset->boneNames[vb.to]);
                ImGui::TableSetColumnIndex(2);
                if (!live || vb.index >= skelComp->boneCount || vb.to >= skelComp->boneCount)
                {
                    ImGui::TextDisabled("-");
                    continue;
                }
                using namespace DirectX;
                const XMMATRIX a = XMLoadFloat4x4(&skelAsset->bindPose[vb.index]) * XMLoadFloat4x4(&skinMats[vb.index]);
                const XMMATRIX b = XMLoadFloat4x4(&skelAsset->bindPose[vb.to]) * XMLoadFloat4x4(&skinMats[vb.to]);
                const float d = XMVectorGetX(XMVector3Length(XMVectorSubtract(a.r[3], b.r[3])));
                ImGui::TextColored(d < 1e-3f ? ImVec4(0.5f, 0.9f, 0.5f, 1.f) : ImVec4(1.f, 0.6f, 0.3f, 1.f), "%.5f", d);
            }
            ImGui::EndTable();
        }
    }

    // ---- Skin matrices (GPU final) ----
    if (ImGui::CollapsingHeader("Skin Matrices (invBind * world)"))
    {
        if (!skinMats || !skelAsset || !skelComp || skelComp->poseByteOffset == ~0u)
        {
            ImGui::TextDisabled("  No active pose data. Ensure animation is playing.");
        }
        else
        {
            static char s_skinFilter[64] = {};
            ImGui::InputText("Filter##skinflt", s_skinFilter, sizeof(s_skinFilter));
            const std::string skinFltStr(s_skinFilter);

            // Summary diagnostics.
            uint32_t nanCount = 0, identityCount = 0, badDetCount = 0;
            float maxTranslation = 0.f;
            uint32_t maxTransBone = 0;
            for (uint32_t b = 0; b < skelComp->boneCount; ++b)
            {
                const XMFLOAT4X4& sm = skinMats[b];
                if (HasBadValues(sm)) ++nanCount;
                if (IsNearIdentity(sm)) ++identityCount;
                float det = Det3x3(sm);
                if (det < 0.5f || det > 2.0f) ++badDetCount;
                float tLen = std::sqrtf(sm._41*sm._41 + sm._42*sm._42 + sm._43*sm._43);
                if (tLen > maxTranslation) { maxTranslation = tLen; maxTransBone = b; }
            }

            if (nanCount > 0)
                ImGui::TextColored(ImVec4(1,0,0,1), "  !! %u bones with NaN/Inf !!", nanCount);
            if (badDetCount > 0)
                ImGui::TextColored(ImVec4(1,0.5f,0,1), "  !! %u bones with bad det3x3 (< 0.5 or > 2.0) !!", badDetCount);
            ImGui::Text("  Identity: %u / %u   maxTranslation: %.3f (bone #%u %s)",
                        identityCount, skelComp->boneCount, maxTranslation, maxTransBone,
                        skelAsset->boneNames[maxTransBone]);

            if (ImGui::BeginTable("SkinMatTable", 7,
                ImGuiTableFlags_Borders    | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_RowBg      | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0, 300)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 32);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("Translation",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Scale(row len)",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Det",  ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Ident",ImGuiTableColumnFlags_WidthFixed, 35);
                ImGui::TableSetupColumn("Flags",ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableHeadersRow();

                for (uint32_t b = 0; b < skelComp->boneCount; ++b)
                {
                    const char* bn = skelAsset->boneNames[b];
                    if (!skinFltStr.empty() &&
                        std::string(bn).find(skinFltStr) == std::string::npos)
                        continue;

                    const XMFLOAT4X4& sm = skinMats[b];
                    bool bad = HasBadValues(sm);
                    float det = Det3x3(sm);
                    XMFLOAT3 scl = ExtractScale(sm);
                    bool ident = IsNearIdentity(sm);
                    bool badDet = (det < 0.5f || det > 2.0f);
                    bool badScl = (std::fabsf(scl.x - 1.f) > 0.05f ||
                                   std::fabsf(scl.y - 1.f) > 0.05f ||
                                   std::fabsf(scl.z - 1.f) > 0.05f);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%u", b);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(bn[0] ? bn : "(?)");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f  %.3f  %.3f", sm._41, sm._42, sm._43);
                    ImGui::TableSetColumnIndex(3);
                    if (badScl)
                        ImGui::TextColored(ImVec4(1,0.5f,0,1), "%.3f %.3f %.3f", scl.x, scl.y, scl.z);
                    else
                        ImGui::Text("%.3f %.3f %.3f", scl.x, scl.y, scl.z);
                    ImGui::TableSetColumnIndex(4);
                    if (badDet)
                        ImGui::TextColored(ImVec4(1,0,0,1), "%.2f", det);
                    else
                        ImGui::Text("%.2f", det);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%s", ident ? "Y" : "");
                    ImGui::TableSetColumnIndex(6);
                    if (bad) ImGui::TextColored(ImVec4(1,0,0,1), "NaN");
                    else if (badDet) ImGui::TextColored(ImVec4(1,0.5f,0,1), "DET");
                    else if (badScl) ImGui::TextColored(ImVec4(1,1,0,1), "SCL");
                }
                ImGui::EndTable();
            }

            // Expandable: the full matrix of the selected bone.
            static int s_inspectBone = 0;
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Inspect bone##skinInspect", &s_inspectBone);
            if (s_inspectBone < 0) s_inspectBone = 0;
            if (s_inspectBone >= (int)skelComp->boneCount) s_inspectBone = (int)skelComp->boneCount - 1;
            {
                const XMFLOAT4X4& sm = skinMats[s_inspectBone];
                ImGui::Text("  skinMat[%d] \"%s\":", s_inspectBone,
                            skelAsset->boneNames[s_inspectBone]);
                ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", sm._11, sm._12, sm._13, sm._14);
                ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", sm._21, sm._22, sm._23, sm._24);
                ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", sm._31, sm._32, sm._33, sm._34);
                ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", sm._41, sm._42, sm._43, sm._44);

                // inverseBindPose and restPoseLocal for comparison.
                if (ImGui::TreeNode("invBindPose / restPoseLocal"))
                {
                    const XMFLOAT4X4& ib = skelAsset->inverseBindPose[s_inspectBone];
                    ImGui::Text("  invBindPose[%d]:", s_inspectBone);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", ib._11, ib._12, ib._13, ib._14);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", ib._21, ib._22, ib._23, ib._24);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", ib._31, ib._32, ib._33, ib._34);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", ib._41, ib._42, ib._43, ib._44);

                    const XMFLOAT4X4& rp = skelAsset->restPoseLocal[s_inspectBone];
                    ImGui::Text("  restPoseLocal[%d]:", s_inspectBone);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", rp._11, rp._12, rp._13, rp._14);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", rp._21, rp._22, rp._23, rp._24);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", rp._31, rp._32, rp._33, rp._34);
                    ImGui::Text("    [%10.5f %10.5f %10.5f %10.5f]", rp._41, rp._42, rp._43, rp._44);
                    ImGui::TreePop();
                }
            }
        }
    }

    // ---- Rest pose identity test ----
    // invBind x bindPose must be identity; otherwise the skeleton data itself is corrupt.
    if (ImGui::CollapsingHeader("Rest Pose Identity Test (invBind * bindPose)"))
    {
        if (!skelAsset)
        {
            ImGui::TextDisabled("  No skeleton asset.");
        }
        else
        {
            uint32_t failCount = 0;
            for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
            {
                XMFLOAT4X4 result;
                XMStoreFloat4x4(&result,
                    XMLoadFloat4x4(&skelAsset->inverseBindPose[b]) *
                    XMLoadFloat4x4(&skelAsset->bindPose[b]));
                if (!IsNearIdentity(result, 0.01f))
                    ++failCount;
            }

            if (failCount == 0)
                ImGui::TextColored(ImVec4(0,1,0,1), "  ALL %u bones PASS (invBind * bindPose ~= I)", skelAsset->boneCount);
            else
                ImGui::TextColored(ImVec4(1,0,0,1), "  %u / %u bones FAIL identity test!", failCount, skelAsset->boneCount);

            if (failCount > 0 && ImGui::TreeNode("Failed bones##identFail"))
            {
                for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
                {
                    XMFLOAT4X4 result;
                    XMStoreFloat4x4(&result,
                        XMLoadFloat4x4(&skelAsset->inverseBindPose[b]) *
                        XMLoadFloat4x4(&skelAsset->bindPose[b]));
                    if (!IsNearIdentity(result, 0.01f))
                    {
                        ImGui::Text("  bone[%u] \"%s\" max err=%.4f",
                                    b, skelAsset->boneNames[b],
                                    [&]() -> float {
                                        const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                                        const float* f = &result._11;
                                        float mx = 0.f;
                                        for (int i = 0; i < 16; ++i)
                                            mx = std::max(mx, std::fabsf(f[i] - id[i]));
                                        return mx;
                                    }());
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    // ---- Mesh skinning info (per-mesh entities) ----
    if (ImGui::CollapsingHeader("Mesh Skinning Info"))
    {
        bool anyMesh = false;
        for (Entity me : m_world->GetEntities())
        {
            const SkeletonRef* ref = m_world->GetComponent<SkeletonRef>(me);
            if (!ref || ref->entity != skelEnt) continue;

            const MeshSkinnedComponent* msc = m_world->GetComponent<MeshSkinnedComponent>(me);
            const SkinningOutputComponent* soc = m_world->GetComponent<SkinningOutputComponent>(me);
            if (!msc) continue;
            anyMesh = true;

            const std::string& mname = m_world->GetName(me);
            ImGui::Text("  Mesh entity #%u \"%s\"", me, mname.c_str());
            ImGui::Text("    vertices: %u   indices: %u   meshDescIdx: %u",
                        msc->vertexCount, msc->indexCount, msc->meshDescriptorIdx);
            if (soc)
            {
                ImGui::Text("    poseByteOfs: 0x%08X  outPosOfs: 0x%08X  outNrmOfs: 0x%08X",
                            soc->poseByteOffset, soc->outPosByteOffset, soc->outNrmByteOffset);
            }
        }
        if (!anyMesh)
            ImGui::TextDisabled("  No mesh entities referencing this skeleton.");
    }

    if(ImGui::CollapsingHeader("Procedural IK")) {
        if(anim) ImGui::Text("MMD mask %016llx | overflow %zu | leg IK %.3f",anim->ikChainMask,anim->ikChainOverflow.size(),anim->legIKWeight);
        if(const auto* rig=m_world->GetComponent<IKRigComponent>(skelEnt)) {
            ImGui::Text("Pelvis %.3f | grounded %s",rig->pelvisOffset,rig->grounded?"yes":"no");
            for(size_t i=0;i<kIKGoalCount;++i) {
                const auto& rt=rig->rt[i];
                ImGui::Text("%s: %s | error %.4fm | weight %.2f | %s",kIKGoalNames[i],IKPathName(rt.path),rt.error,rt.weight,rt.locked?"locked":"free");
            }
        } else ImGui::TextDisabled("No IKRigComponent on skeleton root");
    }

    // ---- IK chains ----
    if (ImGui::CollapsingHeader("IK Chains"))
    {
        if (!skelAsset || skelAsset->ikChains.empty())
        {
            ImGui::TextDisabled("  No IK chains (legacy .iskel or non-PMX model).");
        }
        else
        {
            ImGui::Text("  %zu IK chains:", skelAsset->ikChains.size());
            for (size_t ci = 0; ci < skelAsset->ikChains.size(); ++ci)
            {
                const auto& chain = skelAsset->ikChains[ci];
                const char* ikName     = (chain.ikBoneIndex < skelAsset->boneCount)
                                         ? skelAsset->boneNames[chain.ikBoneIndex] : "?";
                const char* targetName = (chain.targetBoneIndex < skelAsset->boneCount)
                                         ? skelAsset->boneNames[chain.targetBoneIndex] : "?";

                if (ImGui::TreeNode(reinterpret_cast<void*>(static_cast<uintptr_t>(ci)),
                                    "[%zu] %s -> %s  (%u loops, %.3f rad)",
                                    ci, ikName, targetName, chain.loopCount, chain.angleLimit))
                {
                    for (size_t li = 0; li < chain.links.size(); ++li)
                    {
                        const auto& link = chain.links[li];
                        const char* linkName = (link.boneIndex < skelAsset->boneCount)
                                               ? skelAsset->boneNames[link.boneIndex] : "?";
                        if (link.hasAngleLimit)
                            ImGui::Text("    link[%zu] bone %u \"%s\"  limit [%.2f..%.2f, %.2f..%.2f, %.2f..%.2f]",
                                        li, link.boneIndex, linkName,
                                        link.minAngle.x, link.maxAngle.x,
                                        link.minAngle.y, link.maxAngle.y,
                                        link.minAngle.z, link.maxAngle.z);
                        else
                            ImGui::Text("    link[%zu] bone %u \"%s\"  (no limit)", li, link.boneIndex, linkName);
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    // ---- Grant info ----
    if (ImGui::CollapsingHeader("Grant (付与) Info"))
    {
        if (!skelAsset)
        {
            ImGui::TextDisabled("  No skeleton.");
        }
        else
        {
            uint32_t gc = 0;
            for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
                if (skelAsset->grantSource[b] >= 0) ++gc;

            if (gc == 0)
                ImGui::TextDisabled("  No grant relationships.");
            else
            {
                ImGui::Text("  %u grant relationships:", gc);
                for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
                {
                    int32_t gs = skelAsset->grantSource[b];
                    if (gs < 0) continue;
                    const char* dstName = skelAsset->boneNames[b];
                    const char* srcName = (static_cast<uint32_t>(gs) < skelAsset->boneCount)
                                          ? skelAsset->boneNames[gs] : "?";
                    ImGui::Text("    [%u] %s <- [%d] %s  (ratio=%.2f)",
                                b, dstName, gs, srcName, skelAsset->grantRatio[b]);
                }
            }
        }
    }

    ImGui::Separator();

    // ---- Export ----
    auto BuildText = [&]() -> std::string
    {
        std::string out;
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "=== AnimDebug  Entity #%u \"%s\"  (skelEnt=#%u) ===\n",
                 e, ename, skelEnt);
        out += buf;

        // AnimationComponent
        if (anim)
        {
            const ClipLibrary& clipLib = m_renderer->GetClipLibrary();
            snprintf(buf, sizeof(buf),
                     "[AnimComp] clip=#%u  time=%.5f  speed=%.2f  paused=%s  loop=%s\n",
                     anim->primaryClip, anim->primaryTime, anim->speed,
                     anim->paused ? "Y" : "N", anim->looping ? "Y" : "N");
            out += buf;
            if (anim->primaryClip != kInvalidAnimHandle && anim->primaryClip < clipLib.Count())
            {
                const ClipAsset& c = clipLib.Get(anim->primaryClip);
                snprintf(buf, sizeof(buf),
                         "  dur=%.4fs  frames=%u  rate=%.0f  boneCnt(clip)=%u  boneCnt(skel)=%u\n",
                         c.duration, c.frameCount, c.frameRate, c.boneCount,
                         skelComp ? skelComp->boneCount : 0);
                out += buf;
            }
        }
        else out += "[AnimComp] (none)\n";

        // SkeletonComponent
        if (skelComp)
        {
            snprintf(buf, sizeof(buf),
                     "[SkelComp] assetIdx=%u  bones=%u  poseOfs=0x%08X\n",
                     skelComp->assetIndex, skelComp->boneCount, skelComp->poseByteOffset);
            out += buf;
        }
        else out += "[SkelComp] (none)\n";

        // LocalTransform
        if (lt)
        {
            snprintf(buf, sizeof(buf),
                     "[LocalXF] pos=(%.5f,%.5f,%.5f)  rot=(%.5f,%.5f,%.5f,%.5f)  scl=(%.5f,%.5f,%.5f)\n",
                     lt->translation.x, lt->translation.y, lt->translation.z,
                     lt->rotation.x,    lt->rotation.y,    lt->rotation.z, lt->rotation.w,
                     lt->scale.x,       lt->scale.y,       lt->scale.z);
            out += buf;
        }
        else out += "[LocalXF] (none)\n";

        // GlobalTransform
        if (gt)
        {
            const auto& m = gt->matrix;
            snprintf(buf, sizeof(buf),
                     "[GlobalXF] worldPos=(%.5f,%.5f,%.5f)\n"
                     "  [%.5f %.5f %.5f %.5f]\n"
                     "  [%.5f %.5f %.5f %.5f]\n"
                     "  [%.5f %.5f %.5f %.5f]\n"
                     "  [%.5f %.5f %.5f %.5f]\n",
                     m._41, m._42, m._43,
                     m._11,m._12,m._13,m._14,
                     m._21,m._22,m._23,m._24,
                     m._31,m._32,m._33,m._34,
                     m._41,m._42,m._43,m._44);
            out += buf;
        }
        else out += "[GlobalXF] (none)\n";

        // Bone local poses
        if (localPoses && skelAsset)
        {
            snprintf(buf, sizeof(buf), "[BonePoses] count=%u\n", skelAsset->boneCount);
            out += buf;
            out += "  idx  par   name                             pos(x,y,z)                           rot(x,y,z,w)                         scl(x,y,z)\n";
            for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
            {
                const AnimationSystem::LocalPose& p = localPoses[b];
                snprintf(buf, sizeof(buf),
                         "  %-4u %-5d %-32s (%.5f,%.5f,%.5f)  (%.5f,%.5f,%.5f,%.5f)  (%.3f,%.3f,%.3f)\n",
                         b,
                         skelAsset->parentIndex[b],
                         skelAsset->boneNames[b][0] ? skelAsset->boneNames[b] : "?",
                         p.pos.x, p.pos.y, p.pos.z,
                         p.rot.x, p.rot.y, p.rot.z, p.rot.w,
                         p.scl.x, p.scl.y, p.scl.z);
                out += buf;
            }
        }
        else out += "[BonePoses] (none)\n";

        // Grant info
        if (skelAsset)
        {
            out += "\n[Grant Info]\n";
            uint32_t gc = 0;
            for (uint32_t b = 0; b < skelAsset->boneCount; ++b)
            {
                int32_t gs = skelAsset->grantSource[b];
                float   gr = skelAsset->grantRatio[b];
                if (gs < 0 || (gs == 0 && fabsf(gr) < 0.001f)) continue;
                const char* dstName = skelAsset->boneNames[b];
                const char* srcName = (static_cast<uint32_t>(gs) < skelAsset->boneCount)
                                      ? skelAsset->boneNames[gs] : "?";
                snprintf(buf, sizeof(buf), "  [%u] %s <- [%d] %s  ratio=%.3f\n",
                         b, dstName, gs, srcName, gr);
                out += buf;
                ++gc;
            }
            if (gc == 0) out += "  (none)\n";
        }

        // Skin matrices
        if (skinMats && skelComp && skelComp->poseByteOffset != ~0u)
        {
            snprintf(buf, sizeof(buf), "\n[SkinMatrices] count=%u  poseByteOfs=0x%08X\n",
                     skelComp->boneCount, skelComp->poseByteOffset);
            out += buf;
            out += "  idx  name                             trans(x,y,z)                    scale(row-len)        det3x3  flags\n";
            for (uint32_t b = 0; b < skelComp->boneCount; ++b)
            {
                const XMFLOAT4X4& sm = skinMats[b];
                XMFLOAT3 scl = ExtractScale(sm);
                float det = Det3x3(sm);
                bool bad = HasBadValues(sm);
                bool ident = IsNearIdentity(sm);

                const char* flag = "";
                if (bad) flag = "NaN!";
                else if (det < 0.5f || det > 2.0f) flag = "DET!";
                else if (ident) flag = "ident";

                snprintf(buf, sizeof(buf),
                         "  %-4u %-32s (%9.4f,%9.4f,%9.4f)  (%.4f,%.4f,%.4f)  %.3f  %s\n",
                         b,
                         skelAsset ? (skelAsset->boneNames[b][0] ? skelAsset->boneNames[b] : "?") : "?",
                         sm._41, sm._42, sm._43,
                         scl.x, scl.y, scl.z,
                         det, flag);
                out += buf;
            }
        }

        return out;
    };

    if (ImGui::Button("Copy to Clipboard"))
        ImGui::SetClipboardText(BuildText().c_str());

    ImGui::SameLine();

    if (ImGui::Button("Print to Log"))
        LOG_INFO("AnimDebug:\n%s", BuildText().c_str());

    ImGui::End();
}
