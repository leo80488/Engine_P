// MaterialSlotRow.cpp - one material slot as one row. See
// include/Editor/MaterialSlotRow.h.
#include "Editor/MaterialSlotRow.h"

#include "ECS/Components.h"             // MaterialComponent, BlendMode
#include "Editor/EditorTheme.h"
#include "Editor/FontAwesomeIcons.h"
#include "Graphics/ShadingModel.h"
#include "Resource/MaterialDomain.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace Editor
{
namespace
{
    constexpr const char* kBlendNames[] = { "Opaque", "Alpha", "Additive", "Premultiplied", "Multiply",
                                            "Masked", "Dithered", "Transmissive" };

    struct ActionName { MaterialRowAction a; const char* name; };
    constexpr ActionName kActionNames[] = {
        { MaterialRowAction::None,             "none" },
        { MaterialRowAction::Assign,           "assign" },
        { MaterialRowAction::Browse,           "browse" },
        { MaterialRowAction::Clear,            "clear" },
        { MaterialRowAction::Edit,             "edit" },
        { MaterialRowAction::EditHere,         "editHere" },
        { MaterialRowAction::CreateInstance,   "createInstance" },
        { MaterialRowAction::Find,             "find" },
        { MaterialRowAction::CopyPath,         "copyPath" },
        { MaterialRowAction::SaveAsAsset,      "saveAsAsset" },
        { MaterialRowAction::PromoteOverrides, "promote" },
        { MaterialRowAction::ClearOverrides,   "clearOverrides" },
    };

    ImVec4 TagColor(const std::string& tag)
    {
        if (tag == "Instance")    return ImVec4(0.45f, 0.85f, 0.80f, 1.f);
        if (tag == "Custom HLSL") return ImVec4(1.00f, 0.70f, 0.35f, 1.f);
        if (tag == "Graph")       return ImVec4(0.75f, 0.60f, 1.00f, 1.f);
        if (tag == "Inline")      return ImVec4(0.70f, 0.70f, 0.70f, 1.f);
        return ImVec4(0.60f, 0.78f, 1.00f, 1.f);   // Standard
    }

    // "custom_ps_failed" -> "custom ps failed": the code is the short label, the
    // hint's text is the tooltip.
    std::string WarningLabel(const std::string& code)
    {
        std::string s = code;
        std::replace(s.begin(), s.end(), '_', ' ');
        return s;
    }

    // SameLine when a small button labelled `label` still fits before `rightEdge`
    // (screen x); otherwise it starts the next line (a narrow Inspector wraps,
    // never clips).
    void SameLineIfFits(const char* label, float rightEdge)
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.f;
        if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= rightEdge) ImGui::SameLine();
    }
}

const char* MaterialRowActionName(MaterialRowAction a)
{
    for (const ActionName& n : kActionNames)
        if (n.a == a) return n.name;
    return "none";
}

MaterialRowAction MaterialRowActionFromName(const std::string& name)
{
    for (const ActionName& n : kActionNames)
        if (name == n.name) return n.a;
    return MaterialRowAction::None;
}

void DescribeMaterial(const MaterialComponent& m, bool instance, std::string& tag, std::string& summary)
{
    tag = instance ? "Instance"
        : (m.useCustomShader && !m.customShaderPath.empty()) ? "Custom HLSL"
        : "Standard";
    // The blend, then the shading model, joined by a middle dot (U+00B7):
    // "Opaque - Default Lit". Standard is UE's "Default Lit", which also keeps it
    // apart from the tag.
    const auto blend = static_cast<std::uint32_t>(m.userBlendMode);
    summary  = blend < std::size(kBlendNames) ? kBlendNames[blend] : "?";
    summary += " \xC2\xB7 ";
    summary += m.customShadingModel == ShadingModel::Standard ? "Default Lit" : ShadingModelName(m.customShadingModel);
    // The domain only when it is not a surface: "Opaque - Opaque" says nothing.
    if (m.domain == Resource::MaterialDomain::Decal)       summary += " \xC2\xB7 Decal";
    if (m.domain == Resource::MaterialDomain::PostProcess) summary += " \xC2\xB7 Post Process";
    if (m.IsDoubleSided()) summary += " \xC2\xB7 Two Sided";
}

MaterialRowResult DrawMaterialRow(const char* id, const MaterialRowInfo& info)
{
    MaterialRowResult r;
    auto act = [&r](MaterialRowAction a, std::string path = {})
    {
        if (r.action == MaterialRowAction::None) { r.action = a; r.path = std::move(path); }
    };
    auto acceptDrop = [&]()
    {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IMAT_PATH"))
        {
            const bool first = r.action == MaterialRowAction::None;
            act(MaterialRowAction::Assign, static_cast<const char*>(p->Data));
            if (first) r.dropped = true;
        }
        ImGui::EndDragDropTarget();
    };

    ImGui::PushID(id);
    const ImGuiStyle& st     = ImGui::GetStyle();
    const bool        icons  = EditorTheme::GetFonts().icons;
    const bool        hasAsset = !info.path.empty();
    const float       rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

    // ---- The thumbnail: the file on disk, as the Content Browser draws it ----
    const float  thumbSz = ImGui::GetFrameHeight() * 2.f + st.ItemSpacing.y;
    const ImVec2 t0      = ImGui::GetCursorScreenPos();
    const ImVec2 t1(t0.x + thumbSz, t0.y + thumbSz);
    ImGui::InvisibleButton("##thumb", ImVec2(thumbSz, thumbSz));
    const bool   thumbHovered = ImGui::IsItemHovered();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (info.thumb)
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(info.thumb)), t0, t1);
    else
    {
        dl->AddRectFilled(t0, t1, hasAsset ? IM_COL32(52, 64, 78, 255) : IM_COL32(48, 48, 48, 255), 3.f);
        const char*  glyph = icons ? FontAwesome::kPaintBrush : "M";
        const ImVec2 gsz   = ImGui::CalcTextSize(glyph);
        dl->AddText(ImVec2(t0.x + (thumbSz - gsz.x) * 0.5f, t0.y + (thumbSz - gsz.y) * 0.5f),
                    IM_COL32(200, 210, 220, 200), glyph);
    }
    if (thumbHovered) dl->AddRect(t0, t1, IM_COL32(255, 255, 255, 90), 3.f);
    if (thumbHovered && hasAsset && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        act(MaterialRowAction::Edit);
    if (thumbHovered && info.thumb)
    {
        ImGui::BeginTooltip();
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(info.thumb)), ImVec2(128.f, 128.f));
        ImGui::TextDisabled("On disk. Double-click to edit.");
        ImGui::EndTooltip();
    }
    acceptDrop();

    ImGui::SameLine();
    ImGui::BeginGroup();

    // ---- Line 1: the asset field, [Edit], [...] ----
    const std::string editText = icons ? std::string(FontAwesome::kPen) + " Edit" : std::string("Edit");
    const char*  editLabel = editText.c_str();
    const float  editW = ImGui::CalcTextSize(editLabel).x + st.FramePadding.x * 2.f;
    const float  moreW = ImGui::CalcTextSize("...").x + st.FramePadding.x * 2.f;
    const float  fieldW = (std::max)(60.f, ImGui::GetContentRegionAvail().x - editW - moreW -
                                           st.ItemSpacing.x * 2.f);
    char buf[512];
    strncpy_s(buf, sizeof(buf), info.path.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(fieldW);
    const char* hint = info.slotState ? "(no material)" : "Inline material - drop or type an .imat";
    if (ImGui::InputTextWithHint("##asset", hint, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        const std::string typed = buf;
        if (typed.empty() && hasAsset)   act(MaterialRowAction::Clear);
        else if (typed != info.path)     act(MaterialRowAction::Assign, typed);
    }
    if (ImGui::IsItemHovered() && hasAsset && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s\nDrop a material here, or type a path and press Enter.", info.path.c_str());
    acceptDrop();

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasAsset);
    if (ImGui::Button(editLabel)) act(MaterialRowAction::Edit);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(hasAsset ? "Open in the Material Editor" : "No asset: Edit here, or Save as Asset first");

    ImGui::SameLine();
    if (ImGui::Button("...")) ImGui::OpenPopup("##more");
    if (ImGui::BeginPopup("##more"))
    {
        if (ImGui::MenuItem("Browse...")) act(MaterialRowAction::Browse);
        if (ImGui::MenuItem(info.slotState ? "Reset to Model Default" : "Clear", nullptr, false,
                            info.slotState ? std::strcmp(info.slotState, "override") == 0 : hasAsset))
            act(MaterialRowAction::Clear);
        ImGui::Separator();
        if (ImGui::MenuItem("Create Instance...", nullptr, false, hasAsset)) act(MaterialRowAction::CreateInstance);
        if (ImGui::MenuItem("Find in Content Browser", nullptr, false, hasAsset)) act(MaterialRowAction::Find);
        if (ImGui::MenuItem("Copy Path", nullptr, false, hasAsset)) act(MaterialRowAction::CopyPath);
        if (info.entityOverrides >= 0)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Edit on This Entity"))
                act(MaterialRowAction::EditHere);
        }
        ImGui::EndPopup();
    }

    // ---- Line 2: tag and summary ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(TagColor(info.tag), "%s", info.tag.c_str());
    if (!info.summary.empty())
    {
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextDisabled("  \xC2\xB7  %s", info.summary.c_str());
    }
    if (info.slotState)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", info.slotState);
    }
    ImGui::EndGroup();

    // ---- The parent ----
    const float indent = thumbSz + st.ItemSpacing.x;
    if (!info.parent.empty())
    {
        ImGui::Indent(indent);
        if (info.parentResolved)
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "instance of %s", info.parent.c_str());
        else
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "! parent unresolved: %s", info.parent.c_str());
        ImGui::Unindent(indent);
    }

    // ---- The entity's own edits: overrides, unsaved, inline ----
    if (info.entityOverrides >= 0 && (info.entityOverrides > 0 || info.unsaved || !hasAsset))
    {
        ImGui::Indent(indent);
        ImGui::AlignTextToFramePadding();
        if (!hasAsset)
            ImGui::TextDisabled("Inline material");
        else if (info.entityOverrides > 0 || info.unsaved)
        {
            char what[96];
            if (info.entityOverrides > 0)
                std::snprintf(what, sizeof(what), "%d per-entity override%s%s", info.entityOverrides,
                              info.entityOverrides == 1 ? "" : "s", info.unsaved ? " \xC2\xB7 unsaved edits" : "");
            else
                std::snprintf(what, sizeof(what), "unsaved edits on this entity");
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "%s", what);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This entity's material differs from %s.\nThe scene saves the "
                                  "overrides; unsaved edits are lost unless saved.", info.path.c_str());
        }
        SameLineIfFits("Edit here", rightEdge);
        if (ImGui::SmallButton("Edit here")) act(MaterialRowAction::EditHere);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This entity's own material - its per-entity overrides - in a\n"
                              "Material Editor tab");
        if (!hasAsset)
        {
            SameLineIfFits("Save as Asset...", rightEdge);
            if (ImGui::SmallButton("Save as Asset...")) act(MaterialRowAction::SaveAsAsset);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write this material to a new .imat and point the entity at it");
        }
        else if (info.entityOverrides > 0 || info.unsaved)
        {
            SameLineIfFits("Save as Instance...", rightEdge);
            if (ImGui::SmallButton("Save as Instance...")) act(MaterialRowAction::PromoteOverrides);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write this entity's material as a new .imatinst of\n%s\nand point the "
                                  "entity at it (the overrides move into the file)", info.path.c_str());
            SameLineIfFits("Clear##ovr", rightEdge);
            if (ImGui::SmallButton("Clear##ovr")) act(MaterialRowAction::ClearOverrides);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drop this entity's overrides and unsaved edits:\nthe asset's values again");
        }
        ImGui::Unindent(indent);
    }

    // ---- Render.explain's warnings ----
    if (!info.warnings.empty())
    {
        ImGui::Indent(indent);
        for (const auto& [code, text] : info.warnings)
        {
            ImGui::TextColored(ImVec4(1.f, 0.62f, 0.25f, 1.f), "%s %s", icons ? FontAwesome::kWarning : "!",
                               WarningLabel(code).c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
                ImGui::TextUnformatted(text.c_str());
                ImGui::TextDisabled("Render.explain: %s", code.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::Unindent(indent);
    }

    ImGui::PopID();
    return r;
}
}
