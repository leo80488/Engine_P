#pragma once

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include <algorithm>

namespace EditorUI
{
// Shared across panels for this editor session. Right-click a swatch to replace it.
inline ImVec4* ColorSwatches()
{
    static ImVec4 s_colors[] = {
        {1,1,1,1}, {0,0,0,1}, {.5f,.5f,.5f,1}, {1,0,0,1},
        {1,.5f,0,1}, {1,1,0,1}, {0,1,0,1}, {0,1,1,1},
        {0,.4f,1,1}, {0,0,1,1}, {.5f,0,1,1}, {1,0,1,1},
        {1,.5f,.7f,1}, {.4f,.2f,.1f,1}, {.8f,.7f,.5f,1}, {.1f,.2f,.3f,1}
    };
    return s_colors;
}

inline bool ColorEdit4(const char* label, float* color, ImGuiColorEditFlags flags = 0)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool alpha = !(flags & ImGuiColorEditFlags_NoAlpha);
    const int count = alpha ? 4 : 3;
    // Keep HDR values (> 1) even when the caller did not pass ImGuiColorEditFlags_HDR.
    if (color[0] > 1 || color[1] > 1 || color[2] > 1)
        flags |= ImGuiColorEditFlags_HDR;
    flags |= ImGuiColorEditFlags_Float;
    flags &= ~ImGuiColorEditFlags_Uint8;
    const float width = std::max(1.0f, std::min(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x));
    g.NextItemData.ClearFlags();
    const float size = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    ImGui::PushID(label);
    const ImGuiID originalId = ImGui::GetID("original");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    float barWidth = width;
    if (!(flags & ImGuiColorEditFlags_NoLabel) && labelEnd != label)
    {
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float labelWidth = std::min(ImGui::CalcTextSize(label, labelEnd).x,
            std::max(0.0f, width * 0.45f - spacing));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::RenderTextClipped(pos, ImVec2(pos.x + labelWidth, pos.y + size),
            label, labelEnd, nullptr, ImVec2(0, 0.5f));
        ImGui::Dummy(ImVec2(labelWidth, size));
        ImGui::SameLine(0, spacing);
        barWidth = std::max(1.0f, width - labelWidth - spacing);
    }
    bool changed = false;
    const ImVec4 preview(color[0], color[1], color[2], alpha ? color[3] : 1.0f);
    // ColorButton supplies the numeric hover tooltip and alpha checkerboard.
    if (ImGui::ColorButton(label, preview, flags, ImVec2(barWidth, size)))
    {
        for (int i = 0; i < count; ++i) storage->SetFloat(originalId + i, color[i]);
        ImGui::OpenPopup("Color");
    }
    // Accept native color drops onto the swatch, as ImGui's ColorEdit does.
    if (!(flags & ImGuiColorEditFlags_NoDragDrop) &&
        !(g.CurrentItemFlags & ImGuiItemFlags_Disabled) && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_3F))
        {
            std::copy_n(static_cast<const float*>(payload->Data), 3, color);
            changed = true;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F))
        {
            std::copy_n(static_cast<const float*>(payload->Data), count, color);
            changed = true;
        }
        if (changed && (flags & ImGuiColorEditFlags_InputHSV))
            ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], color[0], color[1], color[2]);
        ImGui::EndDragDropTarget();
    }

    ImGuiWindow* pickerWindow = nullptr;
    if (ImGui::BeginPopup("Color"))
    {
        pickerWindow = ImGui::GetCurrentWindow();
        const char* end = ImGui::FindRenderedTextEnd(label);
        if (end != label) ImGui::TextUnformatted(label, end);
        float original[4] = {0, 0, 0, 1};
        for (int i = 0; i < count; ++i) original[i] = storage->GetFloat(originalId + i, color[i]);
        // Scale with the editor font; constrain the picker for small displays.
        const float pickerWidth = std::min(22.0f * ImGui::GetFontSize(),
            ImGui::GetWindowViewport()->WorkSize.x * 0.65f);
        ImGui::SetNextItemWidth(pickerWidth);
        ImGuiColorEditFlags pickerFlags = flags;
        pickerFlags &= ~(ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoLabel |
            ImGuiColorEditFlags_DisplayMask_);
        pickerFlags |= ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV |
            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar;
        changed |= ImGui::ColorPicker4("##picker", color, pickerFlags, original);

        if ((flags & ImGuiColorEditFlags_HDR) && !(flags & ImGuiColorEditFlags_InputHSV))
        {
            float peak = std::max({color[0], color[1], color[2]});
            float intensity = peak;
            ImGui::SetNextItemWidth(pickerWidth);
            if (ImGui::DragFloat("HDR intensity", &intensity, 0.01f, 0.0f, 0.0f, "%.3f"))
            {
                intensity = std::max(0.0f, intensity);
                for (int i = 0; i < 3; ++i)
                    color[i] = peak > 0.0f ? color[i] * (intensity / peak) : intensity;
                changed = true;
            }
        }
        ImGui::SeparatorText("Swatches");
        ImGui::TextDisabled("Click to use; right-click to store (this session)");
        ImVec4* swatches = ColorSwatches();
        for (int i = 0; i < 16; ++i)
        {
            ImGui::PushID(i);
            if (i % 8) ImGui::SameLine();
            if (ImGui::ColorButton("##saved", swatches[i],
                alpha ? ImGuiColorEditFlags_None : ImGuiColorEditFlags_NoAlpha,
                ImVec2(size, size)))
            {
                const float* selected = &swatches[i].x;
                for (int c = 0; c < count; ++c) color[c] = selected[c];
                if (flags & ImGuiColorEditFlags_InputHSV)
                    ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], color[0], color[1], color[2]);
                changed = true;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                swatches[i] = ImVec4(color[0], color[1], color[2], alpha ? color[3] : 1);
                if (flags & ImGuiColorEditFlags_InputHSV)
                    ImGui::ColorConvertHSVtoRGB(color[0], color[1], color[2],
                        swatches[i].x, swatches[i].y, swatches[i].z);
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Restore original"))
        {
            for (int i = 0; i < count; ++i) color[i] = original[i];
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    // Match native ColorEdit's popup forwarding so reflected-field undo can
    // observe activation and the entire picker drag as one active edit.
    if (pickerWindow && g.ActiveId && g.ActiveIdWindow == pickerWindow)
        g.LastItemData.ID = g.ActiveId;
    if (changed && g.LastItemData.ID) ImGui::MarkItemEdited(g.LastItemData.ID);
    return changed;
}

inline bool ColorEdit3(const char* label, float* color, ImGuiColorEditFlags flags = 0)
{
    return ColorEdit4(label, color, flags | ImGuiColorEditFlags_NoAlpha);
}
} // namespace EditorUI
