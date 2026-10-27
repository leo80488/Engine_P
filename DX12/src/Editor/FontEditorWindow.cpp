// FontEditorWindow.cpp - Font Library panel.
//
// UI::FontLibrary owns one Font per TTF, and every Font bakes glyphs ON DEMAND into
// growable R8 SDF atlas pages. This panel:
//   * lists every loaded font (default = entry 0) + its stats,
//   * loads additional fonts by path (same paths UIText.fontPath uses),
//   * prewarms codepoint ranges (avoids first-use hitches for CJK-heavy UI),
//   * previews atlas pages (raw SDF, shown via the red channel),
//   * ensures glyph residency for an arbitrary test string.
// Real rendered previews belong in the scene: a Canvas UIText draws the string
// through the actual SDF pipeline.

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWidgets.h"

#include "imgui/imgui.h"
#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    // ---- Range presets (prewarm multi-select) ----
    struct RangePreset
    {
        const char*        label;
        UI::CodepointRange range;
    };

    // Prewarming is optional: missing glyphs bake on first use. These presets
    // front-load the cost for ranges the UI is known to need.
    constexpr RangePreset kPresets[] = {
        { "ASCII (32-126)",          { 0x0020u, 0x007Eu } }, // prewarmed at load already
        { "Latin-1 Supplement",      { 0x00A0u, 0x00FFu } },
        { "Latin Extended-A",        { 0x0100u, 0x017Fu } },
        { "Hiragana",                { 0x3041u, 0x3096u } },
        { "Katakana",                { 0x30A1u, 0x30FBu } },
        { "CJK Punctuation",         { 0x3000u, 0x303Fu } },
        { "Halfwidth/Fullwidth",     { 0xFF00u, 0xFFEFu } },
        { "CJK 4E00-4FFF (subset)",  { 0x4E00u, 0x4FFFu } },
        { "CJK 4E00-5FFF",           { 0x4E00u, 0x5FFFu } },
    };
    constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

    // ---- Persistent UI state, owned by the editor singleton ----
    struct FontEditorState
    {
        int         selectedFont = 0;
        char        loadPath[256] = "engine/font/HachiMaruPop-Regular.ttf";
        uint32_t    presetMask   = 0u;
        int         customStart  = 0x4E00;
        int         customEnd    = 0x4E7F;
        char        testText[512] =
            "Hello, world! 0123456789\n日本語テキスト、こんにちは世界！";
        int         atlasPage    = 0;
        float       atlasZoom    = 0.5f;
        std::string lastStatus;
        bool        lastOK       = true;
    };

    FontEditorState& State()
    {
        static FontEditorState s_instance;
        return s_instance;
    }
}

void EditorLayer::RenderFontEditorWindow()
{
    if (!m_gfx) return;
    auto& st  = State();
    auto& lib = UI::FontLibrary::Get();

    ImGui::SetNextWindowSize(ImVec2(720.f, 640.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Font Library", &m_showFontEditor))
    {
        ImGui::End();
        return;
    }

    // ---- Loaded fonts ----
    ImGui::SeparatorText("Loaded Fonts");
    const auto& fonts = lib.Fonts();
    if (fonts.empty())
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f),
                           "FontLibrary not initialised — no default font loaded.");
        ImGui::End();
        return;
    }
    st.selectedFont = std::clamp(st.selectedFont, 0,
                                 static_cast<int>(fonts.size()) - 1);

    if (ImGui::BeginTable("##fontlist", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Font", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Base px", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Glyphs",  ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Pages",   ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(fonts.size()); ++i)
        {
            const UI::Font& f = *fonts[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##font%d",
                          f.TTFPath().c_str(), i == 0 ? "  [default]" : "", i);
            if (ImGui::Selectable(label, st.selectedFont == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
            {
                st.selectedFont = i;
                st.atlasPage    = 0;
            }
            ImGui::TableNextColumn(); ImGui::Text("%.0f", f.Metrics().pixelSize);
            ImGui::TableNextColumn(); ImGui::Text("%zu",  f.Glyphs().size());
            ImGui::TableNextColumn(); ImGui::Text("%u",   f.PageCount());
        }
        ImGui::EndTable();
    }

    // ---- Load another font ----
    ImGui::PushItemWidth(-130.f);
    ImGui::InputText("##loadpath", st.loadPath, sizeof(st.loadPath));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Load Font", ImVec2(110.f, 0.f)))
    {
        if (UI::Font* f = lib.GetOrLoad(st.loadPath))
        {
            st.lastStatus = "Loaded " + f->TTFPath();
            st.lastOK     = true;
            for (int i = 0; i < static_cast<int>(fonts.size()); ++i)
                if (fonts[i].get() == f) { st.selectedFont = i; break; }
        }
        else
        {
            st.lastStatus = "Load failed — check the path (see log)";
            st.lastOK     = false;
        }
    }
    if (!st.lastStatus.empty())
    {
        const ImVec4 col = st.lastOK ? ImVec4(0.4f, 0.9f, 0.5f, 1.f)
                                     : ImVec4(1.0f, 0.5f, 0.4f, 1.f);
        ImGui::TextColored(col, "%s", st.lastStatus.c_str());
    }

    UI::Font& font = *fonts[st.selectedFont];

    // ---- Selected font info ----
    ImGui::SeparatorText("Selected Font");
    ImGui::Text("TTF:     %s", font.TTFPath().c_str());
    ImGui::Text("Base:    %.1f px (line %.1f, asc %.1f, desc %.1f, SDF range %.0f)",
                font.Metrics().pixelSize, font.Metrics().lineHeight,
                font.Metrics().ascender, font.Metrics().descender,
                font.Metrics().sdfPixelRange);
    ImGui::Text("Resident: %zu glyphs across %u page(s) (%u KB CPU + GPU each)",
                font.Glyphs().size(), font.PageCount(),
                (UI::Font::kPageSize * UI::Font::kPageSize) / 1024u);

    // ---- Prewarm ranges ----
    if (ImGui::CollapsingHeader("Prewarm Ranges"))
    {
        ImGui::TextDisabled("Optional: glyphs bake on demand at first use. Prewarm only to");
        ImGui::TextDisabled("front-load known-heavy ranges (CJK) and avoid first-use hitches.");
        if (ImGui::BeginTable("##fontranges", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Span", ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < kPresetCount; ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool checked = (st.presetMask & (1u << i)) != 0u;
                if (ImGui::Checkbox(kPresets[i].label, &checked))
                    st.presetMask = checked ? (st.presetMask | (1u << i))
                                            : (st.presetMask & ~(1u << i));
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%u", kPresets[i].range.last - kPresets[i].range.first + 1u);
            }
            ImGui::EndTable();
        }

        ImGui::PushItemWidth(120.f);
        if (ImGui::InputInt("Start##custom", &st.customStart, 1, 16,
                            ImGuiInputTextFlags_CharsHexadecimal))
            st.customStart = std::clamp(st.customStart, 0, 0x10FFFF);
        ImGui::SameLine();
        if (ImGui::InputInt("End##custom", &st.customEnd, 1, 16,
                            ImGuiInputTextFlags_CharsHexadecimal))
            st.customEnd = std::clamp(st.customEnd, 0, 0x10FFFF);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextDisabled("custom U+%04X..U+%04X", st.customStart, st.customEnd);

        if (ImGui::Button("Prewarm Selected Ranges", ImVec2(220.f, 28.f)))
        {
            const size_t before = font.Glyphs().size();
            for (int i = 0; i < kPresetCount; ++i)
                if (st.presetMask & (1u << i))
                    font.EnsureRange(kPresets[i].range.first, kPresets[i].range.last);
            if (st.customStart > 0 && st.customEnd >= st.customStart)
                font.EnsureRange(static_cast<uint32_t>(st.customStart),
                                 static_cast<uint32_t>(st.customEnd));
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Prewarmed %zu new glyphs (%zu resident, %u pages)",
                          font.Glyphs().size() - before, font.Glyphs().size(),
                          font.PageCount());
            st.lastStatus = buf;
            st.lastOK     = true;
        }
    }

    // ---- Ensure a test string ----
    if (ImGui::CollapsingHeader("Test String", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Bakes any missing glyphs of this string into the atlas (what a");
        ImGui::TextDisabled("UIText with this content would do). Preview real rendering by");
        ImGui::TextDisabled("putting the string into a Canvas UIText in the scene.");
        ImGui::InputTextMultiline("##testtext", st.testText, sizeof(st.testText),
                                  ImVec2(-FLT_MIN, 60.f));
        if (ImGui::Button("Ensure Glyphs", ImVec2(160.f, 26.f)))
        {
            font.EnsureGlyphs(st.testText);
            const UI::Vec2 sz = font.Measure(st.testText, UI::kTextRefPx);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Resident. Measures %.0fx%.0f px at %.0f px size.",
                          sz.x, sz.y, UI::kTextRefPx);
            st.lastStatus = buf;
            st.lastOK     = true;
        }
    }

    // ---- Atlas page preview ----
    if (font.IsReady() && ImGui::CollapsingHeader("Atlas Pages", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const int pageCount = static_cast<int>(font.PageCount());
        st.atlasPage = std::clamp(st.atlasPage, 0, pageCount - 1);
        if (pageCount > 1)
            EditorUI::DragInt("Page##atlas", &st.atlasPage, 0, pageCount - 1);
        EditorUI::DragFloat("Zoom##atlas", &st.atlasZoom, 0.1f, 2.0f, "%.2fx");
        ImGui::TextDisabled("Raw SDF view — pages are R8, the field shows in the red channel.");

        const float w = static_cast<float>(UI::Font::kPageSize) * st.atlasZoom;
        const float h = w;
        ImGui::BeginChild("##atlasview", ImVec2(0.f, std::min(h + 20.f, 480.f)), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cur, ImVec2(cur.x + w, cur.y + h), IM_COL32(20, 20, 25, 255));
        const uint64_t srv = font.PageSrvHandle(static_cast<uint32_t>(st.atlasPage));
        if (srv)
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), ImVec2(w, h));
        ImGui::EndChild();
    }

    // ---- Tips ----
    if (ImGui::CollapsingHeader("Tips"))
    {
        ImGui::TextDisabled(
            "* Glyphs bake ON DEMAND: any codepoint a UIText/WorldUIText uses is\n"
            "  rasterised (FreeType -> SDF) the first time and cached in R8 pages.\n"
            "* Pages grow automatically (1024x1024 each) — no fixed glyph budget.\n"
            "* Per-element fonts: set `fontPath` on UIText / WorldUIText /\n"
            "  DamageNumber (Inspector or Engine.SetUIFont in Lua). Empty = default.\n"
            "* Missing codepoints fall back to the default font, then a tofu box.\n"
            "* The default font comes from settings.json \"ui.defaultFont\".");
    }

    ImGui::End();
}
