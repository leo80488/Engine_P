// EntityRefPicker.cpp - Inspector widgets for AttachmentRef + GuidComponent.
//
// An AttachmentRef field is bound by dropping a Hierarchy row (its "ENTITY"
// payload, EditorLayer_Outliner.cpp) onto the field's button; DrawAttachmentRef
// calls BindAndStamp on the dropped entity, which adds a GuidComponent to the
// target if absent. GuidComponent's row shows the GUID hex with Copy and a
// confirmed Regenerate.

#include "Editor/EntityRefPicker.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/ECS.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Editor
{

bool DrawAttachmentRef(const char* label,
                       World& world,
                       Entity selfEntity,
                       AttachmentRef& ref)
{
    bool changed = false;

    // Resolve gives the currently bound runtime entity (it may differ from the cache
    // after a scene reload) and refreshes the cache.
    const Entity bound = ref.Resolve(world);

    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    // Preview string on the drop-target button:
    //   bound + alive            -> "Name #42"
    //   GUID set, no live owner  -> "(unresolved: 0xABCD...)"
    //   no GUID at all           -> "(drag entity here)"
    char preview[160];
    if (bound != NullEntity)
    {
        std::snprintf(preview, sizeof(preview), "%s #%u",
                      world.GetName(bound).c_str(),
                      static_cast<unsigned>(bound));
    }
    else if (ref.HasGuid())
    {
        const std::string s = ref.ownerGuid.ToString();
        std::snprintf(preview, sizeof(preview), "(unresolved: %.8s\xe2\x80\xa6)",
                      s.c_str());
    }
    else
    {
        std::snprintf(preview, sizeof(preview), "(drag entity here)");
    }

    // The button is both the placeholder showing what is bound and the drop target
    // for the Hierarchy's "ENTITY" payload. It is stretched into a wide drop zone,
    // leaving trailing width for the Clear button.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float btnW   = std::max(80.f, availW - 70.f);
    // Dim the button text only when there is no binding, so the placeholder reads
    // as a hint, not a value.
    const bool isPlaceholder = (bound == NullEntity && !ref.HasGuid());
    if (isPlaceholder)
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Button(preview, ImVec2(btnW, 0.f));
    if (isPlaceholder) ImGui::PopStyleColor();

    // Drop target. Self-references are rejected here (not at the source): the drag
    // still completes but the bind is a silent no-op.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("ENTITY"))
        {
            const Entity dropped = *static_cast<const Entity*>(payload->Data);
            if (dropped != NullEntity && dropped != selfEntity)
            {
                ref.BindAndStamp(world, dropped);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Highlight the button border while a compatible payload is dragged anywhere,
    // so the drop sites are obvious.
    if (const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        drag && drag->IsDataType("ENTITY"))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const ImU32 col = ImGui::IsItemHovered()
            ? IM_COL32(120, 220, 140, 255)   // green when actually over us
            : IM_COL32(120, 180, 255, 160);  // blue otherwise
        dl->AddRect(mn, mx, col, 3.0f, 0, 2.0f);
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
    {
        ref.Clear();
        changed = true;
    }

    // Sub-row with the underlying GUID hex (small, dim): a debug aid for copying IDs
    // into bug reports or scripts.
    if (ref.HasGuid())
    {
        ImGui::Indent();
        ImGui::TextDisabled("guid: %s", ref.ownerGuid.ToString().c_str());
        ImGui::Unindent();
    }

    ImGui::PopID();
    return changed;
}

void DrawGuidComponentInspector(GuidComponent& gc, World& /*world*/, Entity self)
{
    if (!gc.guid.IsValid())
    {
        // Stamped via AddComponent without a Generate call: heal silently.
        gc.guid = ECS::Guid::Generate();
        ECS::GuidRegistry::Get().Register(gc.guid, self);
    }

    char buf[40];
    {
        const std::string s = gc.guid.ToString();
        std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    }
    ImGui::PushID("guid_comp_row");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("GUID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-110.f);
    ImGui::InputText("##guid_view", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
    {
        ImGui::SetClipboardText(buf);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New"))
    {
        // Regenerating breaks every AttachmentRef pointing here - only for forking this
        // entity's identity. The confirm popup avoids accidental rerolls.
        ImGui::OpenPopup("##confirm_reroll_guid");
    }
    if (ImGui::BeginPopup("##confirm_reroll_guid"))
    {
        ImGui::TextUnformatted("Regenerate GUID?");
        ImGui::TextDisabled("Breaks every reference currently pointing here.");
        if (ImGui::Button("Regenerate"))
        {
            // Unregister old, generate new, re-register.
            ECS::GuidRegistry::Get().Unregister(gc.guid);
            gc.guid = ECS::Guid::Generate();
            ECS::GuidRegistry::Get().Register(gc.guid, self);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

} // namespace Editor
