// MaterialWidgets.cpp - see the header.
#include "Editor/MaterialWidgets.h"

#include "Editor/ColorWidgets.h"         // EditorUI::ColorEdit3 / ColorEdit4
#include "Editor/TextureViewerWindow.h"  // a slot's right-click: Open in Texture Viewer

#include "imgui.h"

#include <cctype>
#include <cstring>
#include <string>

namespace Editor
{
    bool DrawCustomParamWidget(const CustomParamDrawDesc& d, float* val)
    {
        using V = ShaderReflect::VarType;
        ImGui::PushID(d.name);
        bool changed = false;

        // "color" substring (case-insensitive) -> HDR colour widget, never split.
        bool colorHint = false;
        for (const char* s = d.name; *s && !colorHint; ++s)
            colorHint = _strnicmp(s, "color", 5) == 0;

        const bool isInt  = (d.type == V::Int  || d.type == V::Int2 || d.type == V::Int3 ||
                             d.type == V::Int4 || d.type == V::UInt || d.type == V::UInt2 ||
                             d.type == V::UInt3 || d.type == V::UInt4);
        const bool isBool = (d.type == V::Bool);

        // Split the '|'-separated label into per-component names.
        std::string segs[4];
        int segCount = 0;
        if (d.label && d.label[0])
        {
            const char* p = d.label;
            while (segCount < 4)
            {
                const char* bar = std::strchr(p, '|');
                segs[segCount++].assign(p, bar ? static_cast<size_t>(bar - p)
                                               : std::strlen(p));
                if (!bar) break;
                p = bar + 1;
            }
        }
        const bool split = !isInt && !isBool && !colorHint &&
                           d.numComponents >= 2 && segCount >= 2;

        // Whole-row widgets show the label when it isn't per-component.
        const char* display = (segCount == 1 && !segs[0].empty())
                                  ? segs[0].c_str() : d.name;

        auto rowTooltip = [&]()
        {
            if (!ImGui::IsItemHovered()) return;
            if (d.defaultValue)
                ImGui::SetTooltip("%s\ndefault: %.3g %.3g %.3g %.3g", d.name,
                                  d.defaultValue[0], d.defaultValue[1],
                                  d.defaultValue[2], d.defaultValue[3]);
            else
                ImGui::SetTooltip("%s", d.name);
        };

        if (split)
        {
            // Components the author declared dead (label segment "(unused)") get no
            // widget at all; the value still packs as-is.
            auto isUnusedSeg = [](const std::string& s) -> bool {
                size_t a = s.find_first_not_of(" \t");
                if (a == std::string::npos) return false;
                size_t b = s.find_last_not_of(" \t");
                std::string t = s.substr(a, b - a + 1);
                for (char& ch : t) ch = (char)std::tolower((unsigned char)ch);
                return t == "(unused)" || t == "unused";
            };
            static const char* kComp[4] = { ".x", ".y", ".z", ".w" };
            for (int c = 0; c < d.numComponents; ++c)
            {
                if (c < segCount && isUnusedSeg(segs[c])) continue;
                ImGui::PushID(c);
                std::string rowName = (c < segCount && !segs[c].empty())
                    ? segs[c] : (std::string(d.name) + kComp[c]);
                const float vminC = d.hasRange && d.minValue ? d.minValue[c] : 0.0f;
                const float vmaxC = d.hasRange && d.maxValue ? d.maxValue[c] : 0.0f;
                changed |= ImGui::DragFloat(rowName.c_str(), &val[c], 0.01f, vminC, vmaxC);
                rowTooltip();
                ImGui::PopID();
                // After PopID so the popup ID matches BeginPopup below (the
                // "last item" the click attaches to is still this DragFloat).
                ImGui::OpenPopupOnItemClick("##cpreset", ImGuiPopupFlags_MouseButtonRight);
            }
        }
        else
        {
            const float vmin = d.hasRange && d.minValue ? d.minValue[0] : 0.0f;
            const float vmax = d.hasRange && d.maxValue ? d.maxValue[0] : 0.0f;
            switch (isBool ? V::Bool : (isInt ? d.type
                        : (d.numComponents == 1 ? V::Float
                         : d.numComponents == 2 ? V::Float2
                         : d.numComponents == 3 ? V::Float3 : V::Float4)))
            {
                case V::Bool:
                {
                    bool b = val[0] > 0.5f;
                    if (ImGui::Checkbox(display, &b))
                    { val[0] = b ? 1.0f : 0.0f; changed = true; }
                    break;
                }
                case V::Float:
                    changed = ImGui::DragFloat(display, &val[0], 0.01f, vmin, vmax);
                    break;
                case V::Float2:
                    changed = ImGui::DragFloat2(display, val, 0.01f, vmin, vmax);
                    break;
                case V::Float3:
                    changed = colorHint
                        ? EditorUI::ColorEdit3(display, val,
                            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                        : ImGui::DragFloat3(display, val, 0.01f, vmin, vmax);
                    break;
                case V::Int: case V::UInt:
                {
                    int i = static_cast<int>(val[0]);
                    if (ImGui::DragInt(display, &i, 1.0f, (int)vmin, (int)vmax))
                    { val[0] = static_cast<float>(i); changed = true; }
                    break;
                }
                case V::Int2: case V::UInt2:
                {
                    int iv[2] = { (int)val[0], (int)val[1] };
                    if (ImGui::DragInt2(display, iv))
                    { val[0] = (float)iv[0]; val[1] = (float)iv[1]; changed = true; }
                    break;
                }
                case V::Int3: case V::UInt3:
                {
                    int iv[3] = { (int)val[0], (int)val[1], (int)val[2] };
                    if (ImGui::DragInt3(display, iv))
                    { val[0] = (float)iv[0]; val[1] = (float)iv[1]; val[2] = (float)iv[2]; changed = true; }
                    break;
                }
                case V::Int4: case V::UInt4:
                {
                    int iv[4] = { (int)val[0], (int)val[1], (int)val[2], (int)val[3] };
                    if (ImGui::DragInt4(display, iv))
                    { for (int k = 0; k < 4; ++k) val[k] = (float)iv[k]; changed = true; }
                    break;
                }
                default:
                    changed = colorHint
                        ? EditorUI::ColorEdit4(display, val,
                            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                        : ImGui::DragFloat4(display, val, 0.01f, vmin, vmax);
                    break;
            }
            rowTooltip();
            ImGui::OpenPopupOnItemClick("##cpreset", ImGuiPopupFlags_MouseButtonRight);
        }

        if (d.defaultValue && ImGui::BeginPopup("##cpreset"))
        {
            if (ImGui::MenuItem("Reset to shader default"))
            {
                std::memcpy(val, d.defaultValue, sizeof(float) * 4);
                changed = true;
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    }

    bool DrawTextureSlotWidget(
        const char* label,
        MaterialComponent::TextureMap& slot,
        const MaterialRuntimeTextureView& runtime)
    {
        constexpr float kPreviewSize = 64.f;

        ImGui::PushID(&slot);     // address-based unique id

        // Prefer the UNORM preview handle, which shows sRGB content correctly through
        // ImGui's linear RTV; fall back to the main handle.
        const uint64_t gpuHandle = runtime.previewGpuHandle
            ? runtime.previewGpuHandle : runtime.gpuHandle;
        if (gpuHandle != 0)
        {
            ImGui::Image(
                ImTextureRef(static_cast<ImTextureID>(gpuHandle)),
                ImVec2(kPreviewSize, kPreviewSize));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.f));
            ImGui::Button("##prev", ImVec2(kPreviewSize, kPreviewSize));
            ImGui::PopStyleColor();
        }
        // Right-click the thumbnail -> the Texture Viewer (an explicit id: an Image is
        // not an interactive item).
        if (!slot.name.empty() && ImGui::BeginPopupContextItem("##texslotctx"))
        {
            if (ImGui::MenuItem("Open in Texture Viewer"))
                Editor::TextureViewerWindow::Get().RequestOpen(slot.name);
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);

        char buf[256];
        strncpy_s(buf, sizeof(buf), slot.name.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        bool changed = false;
        if (ImGui::InputText("##path", buf, sizeof(buf)))
        {
            slot.name = buf;
            changed = true;
        }
        ImGui::TextDisabled("UV%u  |  %s", slot.uvset,
            runtime.bindlessIndex >= 0 ? "bound"
            : slot.name.empty()     ? "empty"
                                    : "loading");
        ImGui::EndGroup();

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ITEX_PATH"))
            {
                slot.name = static_cast<const char*>(payload->Data);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        return changed;
    }
}
