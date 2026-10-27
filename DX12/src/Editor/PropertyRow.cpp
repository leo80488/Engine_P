#include "Editor/PropertyRow.h"

#include "imgui/imgui.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace EditorUI
{
namespace
{
    const RowFilter* g_filter = nullptr;

    bool ContainsNoCase(const char* hay, const std::string& needle)
    {
        if (needle.empty()) return true;
        if (!hay) return false;
        for (const char* h = hay; *h; ++h)
        {
            std::size_t i = 0;
            while (i < needle.size() && h[i] &&
                   std::tolower(static_cast<unsigned char>(h[i])) == std::tolower(static_cast<unsigned char>(needle[i])))
                ++i;
            if (i == needle.size()) return true;
        }
        return false;
    }

    bool IsNumber(const std::string& s)
    {
        if (s.empty()) return false;
        char* end = nullptr;
        std::strtod(s.c_str(), &end);
        return end && *end == '\0';
    }
}

bool RowFilter::Matches(const char* label, const char* path, const char* tip) const
{
    return text.empty() || ContainsNoCase(label, text) || ContainsNoCase(path, text) || ContainsNoCase(tip, text);
}

void SetActiveRowFilter(const RowFilter* filter) { g_filter = filter; }
const RowFilter* ActiveRowFilter() { return g_filter; }

std::string LuaLiteral(const std::string& valueText, bool isString)
{
    if (!isString)
    {
        if (valueText == "true" || valueText == "false" || IsNumber(valueText)) return valueText;
        // A vector's text form, x_y_z.
        std::string out = "{";
        std::size_t start = 0;
        bool numbers = true;
        while (true)
        {
            const std::size_t sep = valueText.find('_', start);
            const std::string part = valueText.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            if (!IsNumber(part)) { numbers = false; break; }
            out += (start ? ", " : "") + part;
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        if (numbers) return out + "}";
    }
    std::string out = "\"";
    for (char c : valueText)
    {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

RowAction PropertyRowChrome(const PropertyRowInfo& row, std::string& pasted)
{
    // The marker: a bar on the left edge of the row's widget.
    if (row.modified)
    {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x - 4.f, mn.y + 1.f), ImVec2(mn.x - 2.f, mx.y - 1.f),
                                                  ImGui::GetColorU32(ImGuiCol_CheckMark));
    }
    RowAction action = RowAction::None;
    // The popup's id is scoped by the row's path: rows drawn at one ID-stack level would otherwise share
    // one popup, and a right-click on one row would open (and act on) the first row's menu.
    ImGui::PushID(row.path.c_str());
    if (ImGui::BeginPopupContextItem("##rowctx"))
    {
        ImGui::TextDisabled("%s", row.path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Value")) ImGui::SetClipboardText(row.valueText.c_str());
        const char* clip = ImGui::GetClipboardText();
        if (ImGui::MenuItem("Paste Value", nullptr, false, row.writable && clip && *clip))
        {
            pasted = clip;
            action = RowAction::Paste;
        }
        if (ImGui::MenuItem("Reset to Default", nullptr, false, row.writable && row.modified)) action = RowAction::Reset;
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(row.path.c_str());
        if (!row.luaSetter.empty() && ImGui::MenuItem("Copy Lua")) ImGui::SetClipboardText(row.luaSetter.c_str());
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return action;
}

std::size_t DrawPropertySource(IPropertySource& source, const RowFilter* filter)
{
    std::size_t drawn = 0;
    for (std::size_t i = 0; i < source.RowCount(); ++i)
    {
        if (filter && !filter->Shows(source.Label(i), source.Path(i), source.Tooltip(i), source.Modified(i))) continue;
        ImGui::PushID(static_cast<int>(i));
        source.DrawRow(i);
        ImGui::PopID();
        ++drawn;
    }
    return drawn;
}
}
