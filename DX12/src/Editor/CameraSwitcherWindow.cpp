// CameraSwitcherWindow.cpp - runtime camera-stack inspector / switcher.
//
// Owned by EditorLayer (View -> Camera Switcher). Lists every
// CameraStackChannelSingleton and, per channel, every VirtualCameraComponent
// targeting it with priority, weight, blend state and current blend. Per-row
// buttons:
//   Push - re-enables via Camera::PushVCam (BlendingIn from current blend)
//   Pop  - Camera::PopVCam (BlendingOut)
//   Cut  - Camera::HardCutTo (priority spike + historyValid=false next frame)
//   Sel  - sets the editor selection so the Inspector shows this VCam
// The window never creates or destroys entities; it only flips state on
// existing VCams.

#include "Editor/EditorLayer.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/ECS.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    struct VCamRow
    {
        Entity     entity;
        int        priority;
        float      weight;
        bool       enabled;
        float      currentBlend;
        BlendState state;
        float      blendInDuration;
        float      blendOutDuration;
        BlendCurve curveIn;
        BlendCurve curveOut;
    };

    const char* BlendStateName(BlendState s)
    {
        switch (s)
        {
        case BlendState::Inactive:    return "Inactive";
        case BlendState::BlendingIn:  return "BlendingIn";
        case BlendState::Active:      return "Active";
        case BlendState::BlendingOut: return "BlendingOut";
        }
        return "?";
    }

    ImU32 BlendStateColor(BlendState s)
    {
        switch (s)
        {
        case BlendState::Inactive:    return IM_COL32(120, 120, 120, 255);
        case BlendState::BlendingIn:  return IM_COL32( 90, 180, 255, 255);
        case BlendState::Active:      return IM_COL32( 90, 220, 110, 255);
        case BlendState::BlendingOut: return IM_COL32(230, 160,  60, 255);
        }
        return IM_COL32(200, 200, 200, 255);
    }
}

void EditorLayer::RenderCameraSwitcherWindow()
{
    ImGui::SetNextWindowSize(ImVec2(680.f, 460.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Camera Switcher", &m_showCameraSwitcher))
    {
        ImGui::End();
        return;
    }

    if (!m_world)
    {
        ImGui::TextDisabled("No World attached.");
        ImGui::End();
        return;
    }
    World& world = *m_world;

    // Gather channels first so the panel still renders something useful even
    // when no VCam has been pushed yet (CameraStackChannelSingleton only
    // exists after the first PushVCam / GetOrCreateChannelEntity call).
    struct ChannelRow
    {
        Entity                       channelEntity;
        CameraChannelId              channelId;
        const LiveCameraComponent*   live;
        std::vector<VCamRow>         vcams;
    };
    std::vector<ChannelRow> channels;
    world.ForEach<CameraStackChannelSingleton>(
        [&](Entity e, CameraStackChannelSingleton& s)
    {
        ChannelRow row{};
        row.channelEntity = e;
        row.channelId     = s.channelId;
        row.live          = world.GetComponent<LiveCameraComponent>(e);
        channels.push_back(std::move(row));
    });

    // Collect all VCams across the world, bin them into their channel rows.
    world.ForEach<VirtualCameraComponent>(
        [&](Entity vcam, VirtualCameraComponent& vc)
    {
        auto* prio  = world.GetComponent<VCamPriorityComponent>(vcam);
        auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
        if (!prio || !blend) return;

        ChannelRow* dest = nullptr;
        for (auto& c : channels)
        {
            if (c.channelId == vc.channelId) { dest = &c; break; }
        }
        if (!dest)
        {
            // Orphan channel (VCam exists but no singleton yet). Synthesize a
            // row so the user can still see / push it.
            ChannelRow row{};
            row.channelEntity = NullEntity;
            row.channelId     = vc.channelId;
            row.live          = nullptr;
            channels.push_back(std::move(row));
            dest = &channels.back();
        }

        VCamRow r{};
        r.entity           = vcam;
        r.priority         = prio->priority;
        r.weight           = prio->weight;
        r.enabled          = prio->enabled;
        r.currentBlend     = blend->currentBlend;
        r.state            = blend->state;
        r.blendInDuration  = blend->blendInDuration;
        r.blendOutDuration = blend->blendOutDuration;
        r.curveIn          = blend->curveIn;
        r.curveOut         = blend->curveOut;
        dest->vcams.push_back(r);
    });

    if (channels.empty())
    {
        ImGui::TextDisabled("No camera channels in this scene yet.");
        ImGui::TextDisabled("Add a VirtualCameraComponent to an entity, or call Camera::PushVCam from script.");
        ImGui::End();
        return;
    }

    // Sort each channel's VCams by priority desc, ties by entity id (matches
    // CameraStackTickSystem ordering so what you see here is what wins).
    for (auto& c : channels)
    {
        std::sort(c.vcams.begin(), c.vcams.end(),
            [](const VCamRow& a, const VCamRow& b)
        {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.entity < b.entity;
        });
    }

    // ---- Per-channel panels ----
    for (auto& chan : channels)
    {
        char header[96];
        const char* tag = (chan.channelId == Camera::kMainChannel) ? " (Main)" : "";
        std::snprintf(header, sizeof(header), "Channel 0x%08X%s###chan_%u",
                      chan.channelId, tag, chan.channelEntity);

        if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::Indent();

        // ---- Live resolved values ----
        if (chan.live)
        {
            ImGui::TextDisabled("Live: pos (%.2f, %.2f, %.2f)  fwd (%.2f, %.2f, %.2f)",
                chan.live->position.x, chan.live->position.y, chan.live->position.z,
                chan.live->forward.x,  chan.live->forward.y,  chan.live->forward.z);
            const float fovDeg = chan.live->fov * (180.f / 3.14159265f);
            ImGui::TextDisabled("      fov %.1f°  near %.2f  far %.1f  history:%s",
                fovDeg, chan.live->nearZ, chan.live->farZ,
                chan.live->historyValid ? "OK" : "invalid (cut)");
        }
        else
        {
            ImGui::TextDisabled("Live: (channel singleton not created yet)");
        }

        // ---- Stack table ----
        if (chan.vcams.empty())
        {
            ImGui::TextDisabled("No VCams target this channel.");
            ImGui::Unindent();
            continue;
        }

        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;

        char tableId[32];
        std::snprintf(tableId, sizeof(tableId), "##stack_%u", chan.channelEntity);
        if (ImGui::BeginTable(tableId, 6, tableFlags))
        {
            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Prio",     ImGuiTableColumnFlags_WidthFixed, 48.f);
            ImGui::TableSetupColumn("Wgt",      ImGuiTableColumnFlags_WidthFixed, 48.f);
            ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 96.f);
            ImGui::TableSetupColumn("Blend",    ImGuiTableColumnFlags_WidthFixed, 140.f);
            ImGui::TableSetupColumn("Actions",  ImGuiTableColumnFlags_WidthFixed, 180.f);
            ImGui::TableHeadersRow();

            // The first VCam with enabled==true is the current winner (sorted
            // priority desc); mark it with a leading badge so it's obvious.
            Entity winnerVCam = NullEntity;
            for (const auto& v : chan.vcams)
            {
                if (v.enabled) { winnerVCam = v.entity; break; }
            }

            for (const auto& v : chan.vcams)
            {
                ImGui::PushID(static_cast<int>(v.entity));
                ImGui::TableNextRow();

                // ---- Name + winner badge + selection highlight ----
                ImGui::TableNextColumn();
                if (v.entity == m_selectedEntity)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        IM_COL32(60, 90, 140, 90));
                }
                if (v.entity == winnerVCam)
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.95f, 0.5f, 1.f), "*");
                    ImGui::SameLine();
                }
                else
                {
                    ImGui::TextDisabled(" ");
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(world.GetName(v.entity).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("#%u", v.entity);

                // ---- Priority / weight ----
                ImGui::TableNextColumn();
                ImGui::Text("%d", v.priority);

                ImGui::TableNextColumn();
                ImGui::Text("%.2f", v.weight);

                // ---- State (colored) ----
                ImGui::TableNextColumn();
                ImGui::TextColored(ImColor(BlendStateColor(v.state)).Value,
                                   "%s", BlendStateName(v.state));

                // ---- Blend progress bar ----
                ImGui::TableNextColumn();
                {
                    char overlay[16];
                    std::snprintf(overlay, sizeof(overlay), "%.2f", v.currentBlend);
                    ImGui::ProgressBar(std::clamp(v.currentBlend, 0.f, 1.f),
                                       ImVec2(-FLT_MIN, 0.f), overlay);
                }

                // ---- Actions ----
                ImGui::TableNextColumn();
                {
                    // Push/Pop toggle. Push uses the VCam's own blend durations and curves, so
                    // re-enabling matches what the author configured.
                    if (v.enabled)
                    {
                        if (ImGui::SmallButton("Pop"))
                            Camera::PopVCam(world, v.entity);
                    }
                    else
                    {
                        if (ImGui::SmallButton("Push"))
                        {
                            Camera::PushVCamArgs args{};
                            args.priority         = v.priority;
                            args.weight           = v.weight;
                            args.blendInDuration  = v.blendInDuration;
                            args.blendOutDuration = v.blendOutDuration;
                            args.curveIn          = v.curveIn;
                            args.curveOut         = v.curveOut;
                            args.channelId        = chan.channelId;
                            Camera::PushVCam(world, v.entity, args);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Cut"))
                    {
                        // HardCutTo also force-pushes at priority 10000, which
                        // overrides whatever is currently winning for one frame
                        // and flips historyValid=false (TAA / SSR / fog reset).
                        Camera::HardCutTo(world, v.entity, chan.channelId);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Sel"))
                    {
                        m_selectedEntity = v.entity;
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // ---- Shake quick-poke ----
        if (chan.channelEntity != NullEntity)
        {
            auto* shake = world.GetComponent<CameraShakeComponent>(chan.channelEntity);
            if (shake)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Shake trauma: %.2f", shake->trauma);
                ImGui::SameLine();
                if (ImGui::SmallButton("+0.5"))
                    shake->trauma = std::min(1.f, shake->trauma + 0.5f);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear"))
                    shake->trauma = 0.f;
            }
        }

        ImGui::Unindent();
        ImGui::Spacing();
    }

    ImGui::End();
}
