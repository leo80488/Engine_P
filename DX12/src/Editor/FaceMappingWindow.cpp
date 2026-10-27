// Window > Animation > Face Mapping (see include/Editor/FaceMappingWindow.h).

#include "Editor/FaceMappingWindow.h"

#include "ECS/AnimationComponents.h"   // SkeletonComponent, kInvalidAnimHandle
#include "ECS/FaceTrackingComponent.h"
#include "ECS/FaceTrackingSystem.h"     // AutoProfilePath, LoadProfileFile
#include "ECS/FacialPresets.h"          // Facial::ResolveSkeletonCarrier
#include "FaceTracking/FaceBinding.h"
#include "FaceTracking/FaceMap.h"
#include "FaceTracking/FaceMath.h"
#include "FaceTracking/FaceTrackingService.h"
#include "Resource/SkeletonAsset.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Editor
{
namespace
{
    namespace FT = FaceTracking;
    constexpr const char* kVirtualSource = "virtual";
    constexpr float kDegToRad = FT::FaceMath::kPi / 180.0f;

    const char* GroupName(FT::ChannelGroup g)
    {
        switch (g)
        {
        case FT::ChannelGroup::Eye: return "Eyes";
        case FT::ChannelGroup::Jaw: return "Jaw";
        case FT::ChannelGroup::Mouth: return "Mouth";
        case FT::ChannelGroup::Brow: return "Brows";
        case FT::ChannelGroup::Cheek: return "Cheeks";
        case FT::ChannelGroup::Nose: return "Nose";
        case FT::ChannelGroup::Tongue: return "Tongue";
        case FT::ChannelGroup::Extra: return "Extras";
        }
        return "?";
    }

    void List(const char* label, const std::vector<std::string>& items)
    {
        if (!ImGui::TreeNode(label, "%s (%zu)", label, items.size())) return;
        for (const std::string& s : items) ImGui::BulletText("%s", s.c_str());
        ImGui::TreePop();
    }
}

FaceMappingWindow& FaceMappingWindow::Get()
{
    static FaceMappingWindow s_instance;
    return s_instance;
}

void FaceMappingWindow::Draw(const Context& ctx)
{
    const bool held = ctx.held && ctx.held("Face Mapping");
    if (held && !m_heldBefore)
    {
        m_open = true;
        ImGui::SetNextWindowFocus();
    }
    m_heldBefore = held;
    if (!m_open) return;
    ImGui::SetNextWindowPos(ImVec2(600.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Face Mapping", &m_open))
    {
        ImGui::End();
        return;
    }
    Entity carrier = NullEntity;
    if (ctx.world)
    {
        if (ctx.selected != NullEntity && ctx.world->IsAlive(ctx.selected))
            carrier = Facial::ResolveSkeletonCarrier(*ctx.world, ctx.selected);
        if (carrier == NullEntity)
            if (auto* pool = ctx.world->GetPool<FaceTrackingComponent>(); pool && pool->Size() > 0) carrier = pool->Entities()[0];
    }
    if (carrier == NullEntity || !ctx.skeletons)
    {
        ImGui::TextWrapped("Select a character (an entity with a skeleton).");
        ImGui::End();
        return;
    }
    ImGui::Text("Character: entity %u", static_cast<unsigned>(carrier));
    DrawAutoMap(ctx, carrier);
    ImGui::Separator();
    DrawBinding(ctx, carrier);
    ImGui::Separator();
    DrawVirtualTracker(carrier, ctx);
    ImGui::End();
}

void FaceMappingWindow::DrawAutoMap(const Context& ctx, Entity carrier)
{
    if (!ImGui::CollapsingHeader("Automatic mapping", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const SkeletonComponent* sk = ctx.world->GetComponent<SkeletonComponent>(carrier);
    if (!sk || sk->assetIndex == kInvalidAnimHandle || sk->assetIndex >= ctx.skeletons->Count())
    {
        ImGui::TextDisabled("The skeleton is still loading.");
        return;
    }
    const SkeletonAsset& skel = ctx.skeletons->Get(sk->assetIndex);
    if (ImGui::Button("Auto map") || (m_haveMap && m_mappedFor != carrier))
    {
        std::vector<std::string> morphs;
        for (uint32_t i = 0; i < skel.morphTargetCount; ++i) morphs.emplace_back(skel.morphTargetNames[i]);
        const std::string basePath = FaceTrackingSystem::AutoProfilePath(skel);
        FT::FaceMapProfile base;
        std::string err;
        m_haveMap = false;
        if (FaceTrackingSystem::LoadProfileFile(basePath, base, err))
        {
            m_base = Sys::PathToUtf8(Sys::U8Path(basePath).stem());
            const FT::AutoMapResult r = FT::AutoMapMorphs(morphs, base, m_base);
            m_rules = r.profile.morphs.size();
            m_matches = r.matches;
            m_renamed = r.renamed;
            m_unmapped = r.unmappedChannels;
            m_undriven = r.undrivenMorphs;
            m_mapJson = FT::FaceMapToJson(r.profile);
            m_haveMap = true;
            m_mappedFor = carrier;
            if (m_savePath[0] == '\0') std::snprintf(m_savePath, sizeof(m_savePath), "asset/FaceMaps/entity_%u.ifacemap", static_cast<unsigned>(carrier));
            m_note.clear();
        }
        else
            m_note = "The base mapping did not load: " + err;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u morphs on the model", skel.morphTargetCount);
    if (!m_note.empty()) ImGui::TextWrapped("%s", m_note.c_str());
    if (!m_haveMap) return;

    ImGui::Text("Base: %s, %zu rules of its own; %zu of 52 ARKit channels read, %zu morphs written", m_base.c_str(), m_rules,
                52 - m_unmapped.size(), static_cast<size_t>(skel.morphTargetCount) - m_undriven.size());
    if (ImGui::TreeNode("##matches", "Channels matched to morphs (%zu)", m_matches.size()))
    {
        for (const auto& [channel, morph] : m_matches) ImGui::BulletText("%s -> %s", channel.c_str(), morph.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("##renamed", "Base morphs under the model's spelling (%zu)", m_renamed.size()))
    {
        for (const auto& [from, to] : m_renamed) ImGui::BulletText("%s -> %s", from.c_str(), to.c_str());
        ImGui::TreePop();
    }
    List("Channels nothing reads", m_unmapped);
    List("Morphs nothing writes", m_undriven);

    ImGui::InputText("Save as", m_savePath, sizeof(m_savePath));
    if (ImGui::Button("Save"))
    {
        std::error_code ec;
        const std::filesystem::path parent = Sys::U8Path(m_savePath).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        m_note = Sys::WriteFileBytesAtomic(m_savePath, m_mapJson.data(), m_mapJson.size()) ? std::string("Saved ") + m_savePath
                                                                                             : std::string("Cannot write ") + m_savePath;
    }
    ImGui::SameLine();
    FaceTrackingComponent* ft = ctx.world->GetComponent<FaceTrackingComponent>(carrier);
    ImGui::BeginDisabled(ft == nullptr);
    if (ImGui::Button("Use on this character") && ft)
    {
        ft->profile = m_savePath;
        m_note = std::string("The binding now uses ") + m_savePath + " (save the scene to keep it).";
    }
    ImGui::EndDisabled();
    if (!ft) ImGui::TextDisabled("Add the Face Tracking component (Inspector) to bind this character.");
}

void FaceMappingWindow::DrawBinding(const Context& ctx, Entity carrier)
{
    if (!ImGui::CollapsingHeader("The binding", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const FaceTrackingComponent* ft = ctx.world->GetComponent<FaceTrackingComponent>(carrier);
    if (!ft)
    {
        ImGui::TextDisabled("No Face Tracking component.");
        return;
    }
    ImGui::Text("Source: %s   Profile: %s", ft->source.c_str(), ft->profile.empty() ? "(built-in)" : ft->profile.c_str());
    const FT::BindingRuntime* rt = ft->rt.get();
    if (!rt || !rt->compiled)
    {
        ImGui::TextDisabled(rt && rt->compileFailed ? rt->compileError.c_str() : "Not compiled yet (the source may be missing).");
        return;
    }
    ImGui::Text("%s: %zu targets, authority %.2f, head chain %d bones", rt->profilePath.c_str(), rt->map.targets.size(),
                rt->authorityValue, rt->chainCount);
    if (ImGui::BeginTable("##rules", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0.0f, 220.0f)))
    {
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableHeadersRow();
        for (const FT::CompiledFaceMap::Target& t : rt->map.targets)
        {
            float v = 0.0f;
            for (const auto& [slot, scale] : t.out)
                for (const auto& [s, w] : rt->slotWeights)
                    if (s == slot) v = std::max(v, w);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(t.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::ProgressBar(std::clamp(v * rt->morphAlpha, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
        }
        ImGui::EndTable();
    }
    if (!rt->warnings.empty() && ImGui::TreeNode("##warnings", "Warnings (%zu)", rt->warnings.size()))
    {
        for (const std::string& w : rt->warnings) ImGui::BulletText("%s", w.c_str());
        ImGui::TreePop();
    }
}

void FaceMappingWindow::DrawVirtualTracker(Entity carrier, const Context& ctx)
{
    if (!ImGui::CollapsingHeader("Virtual tracker"))
    {
        return; // hidden: nothing is pushed
    }
    FT::Service& svc = FT::Service::Get();
    ImGui::Checkbox("Drive the 'virtual' source with these sliders", &m_virtualOn);
    FaceTrackingComponent* ft = ctx.world->GetComponent<FaceTrackingComponent>(carrier);
    ImGui::SameLine();
    ImGui::BeginDisabled(ft == nullptr || (ft && ft->source == kVirtualSource));
    if (ImGui::Button("Bind to it") && ft) ft->source = kVirtualSource;
    ImGui::EndDisabled();
    if (ImGui::Button("Neutral"))
    {
        std::fill(std::begin(m_channels), std::end(m_channels), 0.0f);
        m_headYaw = m_headPitch = m_headRoll = m_eyeYaw = m_eyePitch = 0.0f;
    }
    ImGui::SliderFloat("Head yaw", &m_headYaw, -60.0f, 60.0f, "%.0f deg");
    ImGui::SliderFloat("Head pitch", &m_headPitch, -40.0f, 40.0f, "%.0f deg");
    ImGui::SliderFloat("Head roll", &m_headRoll, -30.0f, 30.0f, "%.0f deg");
    ImGui::SliderFloat("Eyes yaw", &m_eyeYaw, -20.0f, 20.0f, "%.0f deg");
    ImGui::SliderFloat("Eyes pitch", &m_eyePitch, -15.0f, 15.0f, "%.0f deg");
    FT::ChannelGroup open = FT::ChannelGroup::Extra;
    bool shown = false;
    for (int ch = 0; ch < FT::kArkitChannelCount; ++ch)
    {
        const FT::ArkitChannelInfo& info = FT::kArkitChannels[ch];
        if (ch == 0 || info.group != FT::kArkitChannels[ch - 1].group)
        {
            if (shown) ImGui::TreePop();
            shown = ImGui::TreeNode(GroupName(info.group));
            open = info.group;
        }
        if (shown && info.group == open)
        {
            const std::string label(info.name);
            ImGui::SliderFloat(label.c_str(), &m_channels[ch], 0.0f, 1.0f, "%.2f");
        }
    }
    if (shown) ImGui::TreePop();

    if (!m_virtualOn || !svc.Enabled()) return;
    FT::SourceStatus st;
    if (!svc.Status(kVirtualSource, st))
    {
        std::string err;
        if (!svc.CreateSynthetic(kVirtualSource, err))
        {
            ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s", err.c_str());
            return;
        }
    }
    FT::FaceFrame f;
    f.faceFound = 1;
    f.confidence = 1.0f;
    for (int ch = 0; ch < FT::kArkitChannelCount; ++ch) f.Set(ch, m_channels[ch]);
    DirectX::XMStoreFloat4(&f.headRot, FT::FaceMath::ComposeHead(m_headYaw * kDegToRad, m_headPitch * kDegToRad, m_headRoll * kDegToRad));
    f.flags |= FT::kFaceHasHead;
    f.SetEye(0, m_eyeYaw * kDegToRad, m_eyePitch * kDegToRad);
    f.SetEye(1, m_eyeYaw * kDegToRad, m_eyePitch * kDegToRad);
    std::string err;
    if (svc.Push(kVirtualSource, f, err) == 0) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s", err.c_str());
}

} // namespace Editor
