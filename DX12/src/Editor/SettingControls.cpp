#include "Editor/SettingControls.h"
#include "Editor/ImGuiWidgets.h"   // EditorUI::DragFloat / DragInt
#include "Editor/PropertyRow.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <string>

namespace EditorUI
{
namespace
{
    // As many decimals as the row's values need: three for 0..1 knobs and small
    // lower bounds (a focus distance from 0.01), two up to 100, one up to 1000,
    // none beyond. ImGui also rounds an edit to this precision.
    const char* FloatFormat(float lo, float hi)
    {
        if (hi <= 1.f || (lo > 0.f && lo < 0.1f)) return "%.3f";
        if (hi <= 100.f) return "%.2f";
        if (hi <= 1000.f) return "%.1f";
        return "%.0f";
    }

    // The row chrome after a row's control: a paste or a reset becomes @p v and counts as a change.
    template <class S>
    bool RowChrome(const Settings::RowT<S>& r, Settings::Value& v)
    {
        PropertyRowInfo row;
        row.path = r.path;
        row.valueText = Settings::ValueText(r, v);
        row.modified = SettingModified(r, v);
        row.luaSetter = std::string("Settings.set(\"") + r.path + "\", " +
                        LuaLiteral(row.valueText, r.kind == Settings::Kind::String || r.kind == Settings::Kind::Enum) + ")";
        std::string pasted;
        switch (PropertyRowChrome(row, pasted))
        {
        case RowAction::Paste:
        {
            Settings::Value nv;
            if (Settings::Coerce(r, Settings::RequestFromText(r, pasted), nv) != Settings::Refusal::None) return false;
            v = nv;
            return true;
        }
        case RowAction::Reset:
            v = Settings::ValueOf(r, S{});
            return true;
        default:
            return false;
        }
    }

    // Whether the active row filter hides this row.
    template <class S>
    bool FilteredOut(const Settings::RowT<S>& r, const Settings::Value& v)
    {
        const RowFilter* f = ActiveRowFilter();
        return f && f->Active() && !f->Shows(SettingTitle(r), r.path, r.spec.tip, SettingModified(r, v));
    }
}

template <class S>
bool SettingModified(const Settings::RowT<S>& r, const Settings::Value& v)
{
    return !Settings::SameValue(r, v, Settings::ValueOf(r, S{}));
}

template <class S>
const char* SettingTitle(const Settings::RowT<S>& r)
{
    return r.spec.title ? r.spec.title : r.key.c_str();
}

template <class S>
std::string SettingLabel(const Settings::RowT<S>& r)
{
    return std::string(SettingTitle(r)) + "##" + r.path;
}

template <class S>
void SettingNotes(const Settings::RowT<S>& r)
{
    if (r.spec.tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", r.spec.tip);
    if (r.spec.flags & Settings::kSettingRestart)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Takes effect the next time the app launches.");
    }
}

template <class S>
bool SettingControl(const Settings::RowT<S>& r, Settings::Value& v)
{
    using Settings::Kind;
    const std::string label = SettingLabel(r);
    bool changed = false;
    switch (r.kind)
    {
    case Kind::Bool:
        changed = ImGui::Checkbox(label.c_str(), &v.b);
        break;
    case Kind::Int:
        changed = DragInt(label.c_str(), &v.i, static_cast<int>(r.spec.lo), static_cast<int>(r.spec.hi), "%d",
                          ImGuiSliderFlags_AlwaysClamp);
        break;
    case Kind::Float:
        changed = DragFloat(label.c_str(), &v.f, r.spec.lo, r.spec.hi, FloatFormat(r.spec.lo, r.spec.hi),
                            ImGuiSliderFlags_AlwaysClamp);
        break;
    case Kind::Enum:
    {
        const int n = static_cast<int>(r.labels.size());
        if (ImGui::BeginCombo(label.c_str(), v.i >= 0 && v.i < n ? r.labels[v.i].c_str() : "?"))
        {
            for (int i = 0; i < n; ++i)
                if (ImGui::Selectable(r.labels[i].c_str(), i == v.i) && i != v.i)
                {
                    v.i = i;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case Kind::Choice:
    {
        const bool named = r.labels.size() == r.values.size();
        const auto name = [&](size_t i) { return named ? r.labels[i] : std::to_string(r.values[i]); };
        std::string preview = std::to_string(v.i);
        for (size_t i = 0; i < r.values.size(); ++i)
            if (r.values[i] == v.i) preview = name(i);
        if (ImGui::BeginCombo(label.c_str(), preview.c_str()))
        {
            for (size_t i = 0; i < r.values.size(); ++i)
                if (ImGui::Selectable(name(i).c_str(), r.values[i] == v.i) && r.values[i] != v.i)
                {
                    v.i = r.values[i];
                    changed = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case Kind::String:
        if (!r.labels.empty())
        {
            if (ImGui::BeginCombo(label.c_str(), v.s.c_str()))
            {
                for (const std::string& l : r.labels)
                    if (ImGui::Selectable(l.c_str(), l == v.s) && l != v.s)
                    {
                        v.s = l;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        }
        else
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", v.s.c_str());
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                v.s = buf;
                changed = true;
            }
        }
        break;
    }
    SettingNotes(r);
    return changed;
}

bool PlayerSetting(const char* path, Settings::CaptureCtx& read, Settings::ApplyCtx& write, const PlayerDraw& draw)
{
    const Settings::Row* r = Settings::FindPlayerRow(path);
    if (!r)
    {
        ImGui::TextDisabled("(no setting %s)", path);
        return false;
    }
    // What runs: the row's capture over the store's value, so a row without a
    // capture (a quality level, ui.language) shows what the store holds, not the
    // struct's default.
    Resource::EngineSettings s;
    Settings::PutValue(*r, Settings::SettingsStore::Get().OwnerValue(*r), s);
    Settings::CapturePlayerRow(*r, s, read);
    Settings::Value v = Settings::ValueOf(*r, s);
    if (FilteredOut(*r, v)) return false;
    bool changed = draw ? draw(*r, v) : SettingControl(*r, v);
    changed |= RowChrome(*r, v);
    if (!changed)
        return false;
    Settings::EditPlayerRow(*r, v, kProjectSettingsWriter, write);
    return true;
}

bool DevSetting(const char* path, Renderer& renderer, const DevDraw& draw)
{
    const Settings::DevRow* r = Settings::FindDevRow(path);
    if (!r)
    {
        ImGui::TextDisabled("(no setting %s)", path);
        return false;
    }
    Resource::EngineDevSettings s;
    Settings::DevCaptureCtx read{ renderer };
    Settings::CaptureDevRow(*r, s, read);
    Settings::Value v = Settings::ValueOf(*r, s);
    if (FilteredOut(*r, v)) return false;
    bool changed = draw ? draw(*r, v) : SettingControl(*r, v);
    changed |= RowChrome(*r, v);
    if (!changed)
        return false;
    Settings::DevApplyCtx write{ renderer };
    Settings::EditDevRow(*r, v, kProjectSettingsWriter, write);
    return true;
}

bool PassSetting(const Settings::PassRow& r)
{
    Settings::Value v = Settings::PassSettingsStore::Get().Effective(r);
    if (FilteredOut(r, v)) return false;
    bool changed = SettingControl(r, v);
    changed |= RowChrome(r, v);
    if (!changed)
        return false;
    Settings::EditPassRow(r, v, kProjectSettingsWriter);
    return true;
}

#define SETTING_CONTROLS_INSTANTIATE(S)                                               \
    template const char* SettingTitle(const Settings::RowT<S>&);                      \
    template std::string SettingLabel(const Settings::RowT<S>&);                      \
    template bool        SettingControl(const Settings::RowT<S>&, Settings::Value&);  \
    template void        SettingNotes(const Settings::RowT<S>&);                      \
    template bool        SettingModified(const Settings::RowT<S>&, const Settings::Value&);
SETTING_CONTROLS_INSTANTIATE(Resource::EngineSettings)
SETTING_CONTROLS_INSTANTIATE(Resource::EngineDevSettings)
SETTING_CONTROLS_INSTANTIATE(Resource::PassSettings)
#undef SETTING_CONTROLS_INSTANTIATE
}
