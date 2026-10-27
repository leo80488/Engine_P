#pragma once

// ImGuiFlow.h - width-aware row / text helpers for editor panels whose width the
// user controls (the Chain Physics Preview panel and its narrow columns): a row
// FLOWS instead of running off the right edge of a child that has no horizontal
// scrollbar.
//   * SameLineIfFits(nextW) - stay on the line only when the next item fits
//     inside the VISIBLE content region, else start a new line.
//   * *W(label)             - the width an item will take, from the style.
//   * DisabledWrapped / ColoredWrapped - a read-out that wraps at the visible
//     right edge.
//
// VISIBLE, not WorkRect: in a child with ImGuiWindowFlags_HorizontalScrollbar
// ImGui grows WorkRect to last frame's content width, so ImGui::TextWrapped
// (which wraps at WorkRect.Max.x) keeps wrapping at whatever the widest thing
// ever was - a self-sustaining width. ContentRegionRect is the window's own
// width (GetContentRegionAvail uses it too), which is what a user can see.
//
// Editor-only (EditorCore). Header-only, inline.

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cstdarg>
#include <string>

namespace EditorUI
{
namespace Flow
{
    // Screen x of the current window's VISIBLE content right edge.
    inline float VisibleRight()
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        return w->ContentRegionRect.Max.x;
    }

    // Width of a Button / SmallButton / Selectable-in-a-row labelled `label`
    // (the "##" suffix hidden). SmallButton has the same horizontal padding.
    inline float ButtonW(const char* label)
    {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.f;
    }

    // Checkbox / RadioButton: the square + the inner spacing + the label.
    inline float CheckboxW(const char* label)
    {
        const float t = ImGui::CalcTextSize(label, nullptr, true).x;
        return ImGui::GetFrameHeight() + (t > 0.f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.f);
    }

    // A framed widget of frame width `frameW` followed by its label.
    inline float LabeledW(float frameW, const char* label)
    {
        const float t = ImGui::CalcTextSize(label, nullptr, true).x;
        return frameW + (t > 0.f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.f);
    }

    inline float TextW(const char* text)
    {
        return ImGui::CalcTextSize(text, nullptr, true).x;
    }

    // SameLine() when an item of width `nextW` still fits on the current line
    // (after the default item spacing), else nothing - the next item starts a new
    // line. Returns true when it stayed on the line.
    inline bool SameLineIfFits(float nextW, float spacing = -1.f)
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const float sp = spacing >= 0.f ? spacing : ImGui::GetStyle().ItemSpacing.x;
        const float x  = w->DC.CursorPosPrevLine.x + sp;
        if (x + nextW <= w->ContentRegionRect.Max.x + 0.5f)
        {
            ImGui::SameLine(0.f, sp);
            return true;
        }
        return false;
    }

    // A frame width that leaves room for `label` on the line: min(want,
    // avail - label) but never below `minW`.
    inline float FitWidth(float want, const char* label, float minW = 60.f)
    {
        const float t = ImGui::CalcTextSize(label, nullptr, true).x;
        const float room = ImGui::GetContentRegionAvail().x -
                           (t > 0.f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.f);
        return std::max(minW, std::min(want, room));
    }

    // The label column a width-aware tree keeps free on the right of its sliders
    // (ChainParamHooks::labelColumn): wide enough for the longest slider labels the
    // chain tree draws, as long as the slider keeps at least 100 px (below that - a
    // column under ~290 px - labels overflow and the column's horizontal scrollbar
    // takes over).
    inline float LabelColumn()
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float want  = std::max(ImGui::CalcTextSize("Structural (along chain)").x,
                                     ImGui::CalcTextSize("Cloth thickness (collision)").x) +
                            ImGui::GetStyle().ItemInnerSpacing.x;
        return std::clamp(want, 110.f, std::max(110.f, avail - 100.f));
    }

    // `text` shortened with "..." to fit `maxW` px (the whole string when it fits).
    // For labels that cannot wrap - a tree node, a combo preview.
    inline std::string Ellipsize(const std::string& text, float maxW)
    {
        if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
        const float dots = ImGui::CalcTextSize("...").x;
        std::string s = text;
        while (!s.empty() && ImGui::CalcTextSize(s.c_str()).x + dots > maxW) s.pop_back();
        return s + "...";
    }

    // Push a wrap position at the VISIBLE right edge (see the file note).
    inline void PushVisibleWrap()
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + std::max(ImGui::GetContentRegionAvail().x, 1.f));
    }

    // The hand-placed '\n' breaks in read-outs / REFLECT_INFO / warnings are laid
    // out for a wide Inspector; wrapped in a narrow column they leave ragged orphan
    // lines. A single break is reflowed into a space; a blank line (paragraph) and a
    // break before a list item ("- ", "* ", "1)", "1.") are kept.
    inline std::string Reflow(const char* s)
    {
        std::string out;
        if (!s) return out;
        for (const char* p = s; *p; ++p)
        {
            if (*p != '\n') { out += *p; continue; }
            const char* q = p + 1;
            while (*q == ' ' || *q == '\t') ++q;
            if (*q == '\n') { out += "\n\n"; p = q; continue; }
            const bool bullet = *q == '-' || *q == '*' ||
                                (*q >= '0' && *q <= '9' && (q[1] == ')' || q[1] == '.'));
            if (bullet) { out += '\n'; continue; }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += ' ';
            p = q - 1;
        }
        return out;
    }

    inline void ColoredWrappedV(const ImVec4& col, const char* fmt, va_list args)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        PushVisibleWrap();
        const char* text = nullptr;
        const char* end  = nullptr;
        ImFormatStringToTempBufferV(&text, &end, fmt, args);
        const std::string flowed = Reflow(std::string(text, end).c_str());
        ImGui::TextUnformatted(flowed.c_str(), flowed.c_str() + flowed.size());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // TextDisabled that wraps at the visible right edge.
    inline void DisabledWrapped(const char* fmt, ...) IM_FMTARGS(1);
    inline void DisabledWrapped(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ColoredWrappedV(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], fmt, args);
        va_end(args);
    }

    // TextColored that wraps at the visible right edge.
    inline void ColoredWrapped(const ImVec4& col, const char* fmt, ...) IM_FMTARGS(2);
    inline void ColoredWrapped(const ImVec4& col, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ColoredWrappedV(col, fmt, args);
        va_end(args);
    }

    // Plain Text that wraps at the visible right edge.
    inline void Wrapped(const char* fmt, ...) IM_FMTARGS(1);
    inline void Wrapped(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ColoredWrappedV(ImGui::GetStyle().Colors[ImGuiCol_Text], fmt, args);
        va_end(args);
    }

    // A Checkbox whose LABEL wraps when the row is too narrow for it (the box, then
    // the label as wrapped text that also toggles on click); the plain ImGui::Checkbox
    // when it fits. The last item is the label then, so a following IsItemHovered()
    // tooltip still works.
    inline bool Checkbox(const char* label, bool* v)
    {
        if (CheckboxW(label) <= ImGui::GetContentRegionAvail().x) return ImGui::Checkbox(label, v);
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##cb", v);
        ImGui::PopID();
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        const char* end = ImGui::FindRenderedTextEnd(label);
        PushVisibleWrap();
        ImGui::TextUnformatted(label, end);
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
        return changed;
    }
}
}
