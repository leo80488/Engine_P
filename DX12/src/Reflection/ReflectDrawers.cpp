// ReflectDrawers.cpp - the editor's widgets for REFLECT_CUSTOM fields (Reflection/ReflectDrawers.h),
// by drawer id: the descriptors in ComponentReflection.h name them, ReflectionEditor.cpp draws them.

#include "Reflection/ReflectDrawers.h"

#include "Reflection/ComponentReflection.h"   // the component types the drawers know
#include "Editor/ImGuiWidgets.h"
#include "imgui/imgui.h"

#include <DirectXMath.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    // Offset editor with a "Snap to Bottom" preset that moves the shape so its lowest point touches
    // the entity pivot (the floor for foot-pivoted characters). Y per shape: Capsule halfHeight +
    // radius, Sphere radius, Box halfExtents.y, Mesh and the rest 0 (the mesh sets its own pivot).
    bool DrawColliderOffset(void* fp, const Reflect::FieldDescriptor& /*f*/)
    {
        auto* offset = static_cast<DirectX::XMFLOAT3*>(fp);
        // Recover the owning ColliderComponent so the preset reads sibling
        // fields and so we can render both Offset + Rotation in this single
        // custom widget block (one widget per REFLECT_CUSTOM site).
        auto* owner  = reinterpret_cast<ColliderComponent*>(
            reinterpret_cast<std::uint8_t*>(offset) - offsetof(ColliderComponent, offset));
        bool changed = false;
        ImGui::PushID("ColliderOffset");
        changed |= ImGui::DragFloat3("Offset", &offset->x, 0.01f, -1000.f, 1000.f, "%.3f");
        if (ImGui::SmallButton("Snap to Bottom"))
        {
            float y = 0.f;
            switch (owner->shape)
            {
                case ColliderComponent::Shape::Capsule: y = owner->halfHeight + owner->radius; break;
                case ColliderComponent::Shape::Sphere:  y = owner->radius;                     break;
                case ColliderComponent::Shape::Box:     y = owner->halfExtents.y;              break;
                case ColliderComponent::Shape::Mesh:    y = 0.f;                               break;
            }
            offset->x = 0.f;
            offset->y = y;
            offset->z = 0.f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Offset")) { *offset = {0.f, 0.f, 0.f}; changed = true; }

        changed |= ImGui::DragFloat3("Rotation (deg)", &owner->rotationEulerDeg.x,
                                     0.5f, -360.f, 360.f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Rot")) { owner->rotationEulerDeg = {0.f, 0.f, 0.f}; changed = true; }
        // Capsule-on-its-side presets: Jolt capsules are Y-axis primitives, so 90 deg about Z lays one
        // along X.
        ImGui::SameLine();
        if (ImGui::SmallButton("Lay X")) { owner->rotationEulerDeg = {0.f, 0.f, 90.f};  changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Lay Z")) { owner->rotationEulerDeg = {90.f, 0.f, 0.f};  changed = true; }

        ImGui::PopID();
        return changed;
    }

    // Hooked on tags, which sits at offset 0, so the field pointer is the component pointer. Tags
    // are picked from the project-wide TagRegistry (asset/tags.txt), not typed, which rules out
    // typos; "New..." adds a tag to the registry and to this entity, "Manage" edits the registry.
    bool DrawTagEditor(void* fp, const Reflect::FieldDescriptor& /*f*/)
    {
        auto* tc  = reinterpret_cast<TagComponent*>(fp);
        auto& reg = TagRegistry::Get();
        bool changed = false;
        ImGui::PushID("TagEditor");

        // ---- Current tags, each with a remove button ----
        for (uint32_t i = 0; i < tc->count; )
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            ImGui::SameLine();
            const bool registered = reg.Contains(tc->tags[i]);
            if (registered)
                ImGui::TextUnformatted(tc->tags[i]);
            else
            {
                // Stamped by a script or by content but missing from the registry: flag it and offer one-click
                // registration.
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "%s", tc->tags[i]);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Not in the tag registry (asset/tags.txt)");
                ImGui::SameLine();
                if (ImGui::SmallButton("+reg")) reg.Add(tc->tags[i]);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
            {
                // Copy first: Remove() shifts the array the argument would alias.
                char victim[TagComponent::LEN];
                std::memcpy(victim, tc->tags[i], TagComponent::LEN);
                tc->Remove(victim);
                changed = true;
                ImGui::PopID();
                continue;   // same index now holds the next tag
            }
            ImGui::PopID();
            ++i;
        }

        // ---- Picker: registry tags not already on this entity ----
        const bool full = tc->count >= TagComponent::MAX;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::BeginCombo("##pickTag", full ? "(full)" : "Add tag..."))
        {
            for (const std::string& t : reg.Tags())
            {
                if (tc->Has(t.c_str())) continue;
                if (ImGui::Selectable(t.c_str()) && !full)
                {
                    tc->Add(t.c_str());
                    changed = true;
                }
            }
            if (reg.Tags().empty())
                ImGui::TextDisabled("(registry empty — use New...)");
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("New...")) ImGui::OpenPopup("NewTagPopup");
        ImGui::SameLine();
        if (ImGui::SmallButton("Manage")) ImGui::OpenPopup("ManageTagsPopup");
        ImGui::SameLine();
        ImGui::TextDisabled("%u/%u", tc->count, TagComponent::MAX);

        // ---- New tag: registers it in asset/tags.txt and stamps the entity ----
        if (ImGui::BeginPopup("NewTagPopup"))
        {
            static char s_newTag[TagComponent::LEN] = {};
            ImGui::TextDisabled("New tag (added to registry + this entity)");
            ImGui::SetNextItemWidth(180.f);
            const bool entered = ImGui::InputText("##newTagName", s_newTag, sizeof(s_newTag),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::SmallButton("Create") || entered) && s_newTag[0])
            {
                if (reg.Add(s_newTag))
                {
                    if (!full && tc->Add(s_newTag)) changed = true;
                    s_newTag[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        // ---- Registry manager (Unity TagManager) ----
        if (ImGui::BeginPopup("ManageTagsPopup"))
        {
            ImGui::TextDisabled("Tag registry (asset/tags.txt)");
            ImGui::Separator();
            // Copy: Remove() mutates the vector being iterated.
            const std::vector<std::string> tags = reg.Tags();
            for (const std::string& t : tags)
            {
                ImGui::PushID(t.c_str());
                ImGui::Bullet(); ImGui::SameLine();
                ImGui::TextUnformatted(t.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) reg.Remove(t);   // registry only; entities keep theirs
                ImGui::PopID();
            }
            if (tags.empty()) ImGui::TextDisabled("(empty)");
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    }

    struct NamedDrawer
    {
        const char*       id;
        Reflect::DrawerFn fn;
    };

    const NamedDrawer kBuiltIn[] = {
        { "colliderOffset",  &DrawColliderOffset },
        { "tags",            &DrawTagEditor },
    };

    // A module's drawers, registered at its editor module's start (main thread).
    std::vector<NamedDrawer>& Registered()
    {
        static std::vector<NamedDrawer> s_drawers;
        return s_drawers;
    }
}

namespace Reflect
{
    void RegisterDrawer(const char* id, DrawerFn fn)
    {
        for (NamedDrawer& d : Registered())
            if (std::strcmp(d.id, id) == 0) { d.fn = fn; return; }
        Registered().push_back({ id, fn });
    }

    DrawerFn FindDrawer(const char* id)
    {
        if (!id) return nullptr;
        for (const NamedDrawer& d : kBuiltIn)
            if (std::strcmp(d.id, id) == 0) return d.fn;
        for (const NamedDrawer& d : Registered())
            if (std::strcmp(d.id, id) == 0) return d.fn;
        return nullptr;
    }
}
