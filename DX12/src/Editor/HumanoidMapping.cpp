// HumanoidMapping - see the header.

#include "Editor/HumanoidMapping.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "Resource/SkeletonAsset.h"
#include "System/PathUtil.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace Editor
{
    bool SaveHumanoidProfileToIskel(const std::string& path, const HumanoidProfile& hp, std::string* why)
    {
        auto fail = [why](std::string text)
        {
            if (why) *why = std::move(text);
            return false;
        };
        if (Sys::ThroughReparsePoint(path))
            return fail("'" + path + "' is reached through a junction (another checkout's content): an editor never "
                        "writes there");
        // Read where the engine would (a worktree's shared content root too);
        // the write below is always THIS checkout's copy of the path.
        std::vector<uint8_t> d;
        if (!Resource::AssetFS::Get().ReadFile(path, d)) return fail("cannot open '" + path + "'");

        // The section is the LAST sizeof(HumanoidProfile) bytes of the payload, gated by
        // ISKEL_FLAG_HUMANOID in SkeletonMetadata.reserved[1]. A current-ABI section is
        // assumed when the flag is set: a file with an older, shorter profile struct must
        // be re-imported first.
        constexpr size_t kHeader = sizeof(Resource::AssetHeader);        // 24
        constexpr size_t kMeta   = sizeof(Resource::SkeletonMetadata);   // 16
        if (d.size() < kHeader + kMeta || *reinterpret_cast<const uint32_t*>(d.data()) != Resource::MAGIC_SKELETON)
            return fail("'" + path + "' is not a skeleton (.iskel)");

        const uint32_t flags = *reinterpret_cast<const uint32_t*>(d.data() + kHeader + 12);   // reserved[1]
        if (flags & Resource::ISKEL_FLAG_HUMANOID)
        {
            if (d.size() < kHeader + kMeta + sizeof(HumanoidProfile))
                return fail("'" + path + "' says it has a humanoid section but is too short for one");
            std::memcpy(d.data() + d.size() - sizeof(HumanoidProfile), &hp, sizeof(HumanoidProfile));
        }
        else
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&hp);
            d.insert(d.end(), p, p + sizeof(HumanoidProfile));
            *reinterpret_cast<uint32_t*>(d.data() + kHeader + 12) |= Resource::ISKEL_FLAG_HUMANOID;
            reinterpret_cast<Resource::AssetHeader*>(d.data())->dataSize += sizeof(HumanoidProfile);
        }
        Sys::CreateParentDirs(path);   // a payload-only folder a shared worktree does not hold
        if (!Sys::WriteFileBytesAtomic(path, d.data(), d.size())) return fail("cannot write '" + path + "'");
        return true;
    }

    namespace
    {
        // Case-insensitive (ASCII) substring filter; UTF-8 (JP) matches byte-wise.
        bool BoneNameMatches(const char* name, const char* filter)
        {
            if (!filter[0]) return true;
            std::string a(name), b(filter);
            for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a.find(b) != std::string::npos;
        }

        using H = HumanoidBone;
        const H kTorso[] = { H::Hips, H::Spine, H::Chest, H::UpperChest, H::Neck, H::Head, H::LeftEye, H::RightEye,
                             H::Jaw };
        const H kArmL[]  = { H::LeftShoulder, H::LeftUpperArm, H::LeftLowerArm, H::LeftHand };
        const H kArmR[]  = { H::RightShoulder, H::RightUpperArm, H::RightLowerArm, H::RightHand };
        const H kLegL[]  = { H::LeftUpperLeg, H::LeftLowerLeg, H::LeftFoot, H::LeftToes };
        const H kLegR[]  = { H::RightUpperLeg, H::RightLowerLeg, H::RightFoot, H::RightToes };
        const H kFingL[] = { H::LeftThumbMetacarpal, H::LeftThumbProximal, H::LeftThumbDistal,
                             H::LeftIndexProximal, H::LeftIndexIntermediate, H::LeftIndexDistal,
                             H::LeftMiddleProximal, H::LeftMiddleIntermediate, H::LeftMiddleDistal,
                             H::LeftRingProximal, H::LeftRingIntermediate, H::LeftRingDistal,
                             H::LeftLittleProximal, H::LeftLittleIntermediate, H::LeftLittleDistal };
        const H kFingR[] = { H::RightThumbMetacarpal, H::RightThumbProximal, H::RightThumbDistal,
                             H::RightIndexProximal, H::RightIndexIntermediate, H::RightIndexDistal,
                             H::RightMiddleProximal, H::RightMiddleIntermediate, H::RightMiddleDistal,
                             H::RightRingProximal, H::RightRingIntermediate, H::RightRingDistal,
                             H::RightLittleProximal, H::RightLittleIntermediate, H::RightLittleDistal };
        const HumanoidRoleGroup kGroups[] = {
            { "Torso / Head",  kTorso, std::size(kTorso), true },
            { "Left Arm",      kArmL,  std::size(kArmL),  true },
            { "Right Arm",     kArmR,  std::size(kArmR),  true },
            { "Left Leg",      kLegL,  std::size(kLegL),  true },
            { "Right Leg",     kLegR,  std::size(kLegR),  true },
            { "Left Fingers",  kFingL, std::size(kFingL), false },
            { "Right Fingers", kFingR, std::size(kFingR), false },
        };
    }

    const char* HumanoidConventionName(std::uint8_t convention)
    {
        switch (static_cast<HumanoidConvention>(convention))
        {
        case HumanoidConvention::Unknown:         return "unknown";
        case HumanoidConvention::PmxJp:           return "PMX (JP)";
        case HumanoidConvention::Mixamo:          return "Mixamo";
        case HumanoidConvention::UnrealMannequin: return "UE Mannequin";
        case HumanoidConvention::GenericEnglish:  return "generic";
        case HumanoidConvention::Vrm0:            return "VRM 0";
        case HumanoidConvention::Vrm1:            return "VRM 1";
        }
        return "?";
    }

    const HumanoidRoleGroup* HumanoidRoleGroups(std::size_t& count)
    {
        count = std::size(kGroups);
        return kGroups;
    }

    bool HumanoidBonePickerRow(const char* label, bool required, const SkeletonAsset& sk, std::int32_t& ioBone,
                               char* filter, std::size_t filterCap, const char* noneLabel, bool* labelClicked)
    {
        bool changed = false;
        ImGui::PushID(label);

        // The label column, then the bone button: 160 + 240 px where there is room (the
        // Retarget window), shared out where there is not (the Skeleton Editor's pane).
        const float startX = ImGui::GetCursorPosX();
        const float labelW = std::min(160.f, ImGui::GetContentRegionAvail().x * 0.45f);
        const bool missing = required && ioBone < 0;
        if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.3f, 1.f));
        if (labelClicked)
        {
            const float starW = required ? ImGui::CalcTextSize("*").x + ImGui::GetStyle().ItemSpacing.x : 0.f;
            if (ImGui::Selectable(label, false, 0, ImVec2(std::max(20.f, labelW - starW - 4.f), 0.f))) *labelClicked = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select its bone");
        }
        else
            ImGui::TextUnformatted(label);
        if (missing) ImGui::PopStyleColor();
        if (required) { ImGui::SameLine(); ImGui::TextDisabled("*"); }

        ImGui::SameLine(startX + labelW);
        const char* cur = (ioBone >= 0 && ioBone < static_cast<std::int32_t>(sk.boneCount)) ? sk.boneNames[ioBone]
                                                                                               : noneLabel;
        if (ImGui::Button(cur, ImVec2(std::min(240.f, std::max(40.f, ImGui::GetContentRegionAvail().x)), 0.f)))
        {
            filter[0] = '\0';
            ImGui::OpenPopup("pick");
        }
        if (ImGui::BeginPopup("pick"))
        {
            ImGui::SetNextItemWidth(220.f);
            ImGui::InputTextWithHint("##filter", "filter...", filter, filterCap);
            if (ImGui::Selectable(noneLabel)) { ioBone = -1; changed = true; }
            ImGui::Separator();
            ImGui::BeginChild("bones", ImVec2(240.f, 260.f));
            // Real bones only: a virtual bone has no role.
            for (std::uint32_t i = 0; i < sk.RealBoneCount(); ++i)
            {
                if (!BoneNameMatches(sk.boneNames[i], filter)) continue;
                const bool sel = (static_cast<std::int32_t>(i) == ioBone);
                if (ImGui::Selectable(sk.boneNames[i], sel))
                {
                    ioBone  = static_cast<std::int32_t>(i);
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return changed;
    }

    bool DrawHumanoidRoleRows(SkeletonAsset& sk, char* filter, std::size_t filterCap,
                              const std::function<void(HumanoidBone, int)>& onRoleClicked,
                              const std::function<void()>& beforeChange)
    {
        HumanoidProfile& hp = sk.humanoid;
        bool changed = false;
        // A picker edits a copy: the profile changes only after beforeChange saw it.
        auto apply = [&](std::int32_t& slot, std::int32_t value)
        {
            if (!changed && beforeChange) beforeChange();
            slot    = value;
            changed = true;
        };
        auto group = [&](const HumanoidRoleGroup& g)
        {
            if (!ImGui::TreeNodeEx(g.name, g.defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
            for (std::size_t i = 0; i < g.count; ++i)
            {
                const HumanoidBone  role = g.roles[i];
                const std::uint32_t ri   = static_cast<std::uint32_t>(role);
                std::int32_t bone = hp.boneForRole[ri];
                bool clicked = false;
                if (HumanoidBonePickerRow(HumanoidRoleName(role), IsRequiredHumanoidBone(role), sk, bone, filter,
                                          filterCap, "(none)", onRoleClicked ? &clicked : nullptr) &&
                    bone != hp.boneForRole[ri])
                    apply(hp.boneForRole[ri], bone);
                if (clicked) onRoleClicked(role, hp.boneForRole[ri]);
            }
            ImGui::TreePop();
        };

        std::size_t n = 0;
        const HumanoidRoleGroup* groups = HumanoidRoleGroups(n);
        group(groups[0]);   // Torso / Head

        // Hips-translation split bone (MMD: root motion on the center bone).
        if (ImGui::TreeNodeEx("Root Motion", ImGuiTreeNodeFlags_DefaultOpen))
        {
            std::int32_t bone = hp.hipsTranslationBone;
            if (HumanoidBonePickerRow("hipsTranslation", false, sk, bone, filter, filterCap, "(same as hips)") &&
                bone != hp.hipsTranslationBone)
                apply(hp.hipsTranslationBone, bone);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bone that receives root TRANSLATION when it differs from the\n"
                                  "hips rotation bone. MMD: rotation on 下半身, translation on センター.");
            ImGui::TreePop();
        }
        for (std::size_t g = 1; g < n; ++g) group(groups[g]);

        if (changed)
        {
            hp.userEdited = 1;
            Resource::HumanoidProfileBuilder::FinalizeDerived(sk);
        }
        return changed;
    }
}
