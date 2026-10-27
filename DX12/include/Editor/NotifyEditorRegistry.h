#pragma once

// NotifyEditorRegistry - per-category property-panel drawers for NotifyTrackEditor.
//
// The track editor does not know a category's fields: subsystem owners (Hitbox / VFX /
// Audio / ...) register a callback that draws the per-Notify panel with ImGui; the editor
// owns only the selection and the host window. Header-only singleton keyed by
// NotifyCategory: a new category = extend the enum, register a draw fn. With no callback
// registered, a generic key/value editor over the PropertyBag is drawn.

#include "ECS/NotifyTypes.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>

class NotifyEditorRegistry
{
public:
    using DrawFn = std::function<void(Notify&)>;

    static NotifyEditorRegistry& Get()
    {
        static NotifyEditorRegistry s_inst;
        return s_inst;
    }

    static void Register(NotifyCategory cat, DrawFn fn)
    {
        Get().m_drawers[static_cast<size_t>(cat)] = std::move(fn);
    }

    static void RenderEditor(Notify& n)
    {
        auto& self = Get();
        const auto idx = static_cast<size_t>(n.category);
        if (idx < self.m_drawers.size() && self.m_drawers[idx]) {
            self.m_drawers[idx](n);
            return;
        }
        DrawGenericPropertyBag(n.params);
    }

private:
    NotifyEditorRegistry() = default;

    // Fallback panel: a typed widget per PropertyBag entry for the alternative actually
    // stored; a "key + type" row at the bottom adds entries.
    static void DrawGenericPropertyBag(PropertyBag& bag)
    {
        ImGui::TextDisabled("(generic params)");
        ImGui::Separator();

        std::string toRemove;
        for (auto& [key, value] : bag.values)
        {
            ImGui::PushID(key.c_str());
            ImGui::SetNextItemWidth(120.f);
            ImGui::Text("%s", key.c_str());
            ImGui::SameLine();

            std::visit([&](auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int>) {
                    ImGui::DragInt("##v", &v);
                } else if constexpr (std::is_same_v<T, float>) {
                    ImGui::DragFloat("##v", &v, 0.01f);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    char buf[256];
                    const size_t n = std::min(v.size(), sizeof(buf) - 1);
                    std::memcpy(buf, v.data(), n);
                    buf[n] = '\0';
                    if (ImGui::InputText("##v", buf, sizeof(buf)))
                        v.assign(buf);
                } else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>) {
                    ImGui::DragFloat3("##v", &v.x, 0.01f);
                } else if constexpr (std::is_same_v<T, DirectX::XMFLOAT4>) {
                    ImGui::DragFloat4("##v", &v.x, 0.01f);
                }
            }, value);

            ImGui::SameLine();
            if (ImGui::SmallButton("x")) toRemove = key;
            ImGui::PopID();
        }
        if (!toRemove.empty()) bag.values.erase(toRemove);

        // "Add new param" row - at the bottom so it does not shuffle when the map's iteration
        // order changes.
        ImGui::Separator();
        static char s_newKey[64] = {};
        static int  s_newType   = 1;  // float by default
        const char* kKinds[]    = { "int", "float", "string", "vec3", "vec4" };
        ImGui::SetNextItemWidth(120.f);
        ImGui::InputText("##nk", s_newKey, sizeof(s_newKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        ImGui::Combo("##nt", &s_newType, kKinds, IM_ARRAYSIZE(kKinds));
        ImGui::SameLine();
        if (ImGui::SmallButton("+") && s_newKey[0] != '\0' && !bag.values.count(s_newKey)) {
            switch (s_newType) {
                case 0: bag.values[s_newKey] = int{0}; break;
                case 1: bag.values[s_newKey] = 0.f; break;
                case 2: bag.values[s_newKey] = std::string{}; break;
                case 3: bag.values[s_newKey] = DirectX::XMFLOAT3{}; break;
                case 4: bag.values[s_newKey] = DirectX::XMFLOAT4{}; break;
            }
            s_newKey[0] = '\0';
        }
    }

    std::array<DrawFn, static_cast<size_t>(NotifyCategory::COUNT)> m_drawers{};
};
