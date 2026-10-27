// EditorTheme.cpp - ImGui style + the editor's merged font chain. The header has
// the chain order and why there is no glyph-range list.
#include "Editor/EditorTheme.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
    // ---- Font files ----
    // engine/editor/ is the editor-only subtree tools/package_game.py skips.
    constexpr const char* kNotoLatin     = "engine/editor/fonts/Noto_Sans/static/NotoSans-Regular.ttf";
    constexpr const char* kNotoLatinBold = "engine/editor/fonts/Noto_Sans/static/NotoSans-SemiBold.ttf";
    constexpr const char* kNotoTC        = "engine/editor/fonts/Noto_Sans_TC/static/NotoSansTC-Regular.ttf";
    constexpr const char* kNotoJP        = "engine/editor/fonts/Noto_Sans_JP/static/NotoSansJP-Regular.ttf";
    // OPTIONAL tail of the chain: loaded when present, silently skipped when not.
    // TC + JP leave ~2k Simplified-only hanzi and all of Hangul uncovered; dropping
    // Google's Noto Sans SC / KR download beside the other two folders completes them
    // with no code change.
    constexpr const char* kNotoSC        = "engine/editor/fonts/Noto_Sans_SC/static/NotoSansSC-Regular.ttf";
    constexpr const char* kNotoKR        = "engine/editor/fonts/Noto_Sans_KR/static/NotoSansKR-Regular.ttf";
    constexpr const char* kIconFont      = "engine/font/fontawesome/Font Awesome 7 Free-Solid-900.otf";
    // Fallback face (Japanese gothic), used only when the Noto files are missing.
    constexpr const char* kLegacyCjk     = "engine/font/HigashiOme-Gothic-1.3i.ttf";

    // Font Awesome lives in the Private Use Area. The icon source takes all of
    // it; every text source excludes it, so an icon can never be shadowed by a
    // text face that happens to carry a PUA glyph.
    const ImWchar kPuaRange[]   = { 0xE000, 0xF8FF, 0 };

    // File bytes, owned HERE rather than by the atlas: the CJK faces are merged into
    // both the regular and the bold chain, and FontDataOwnedByAtlas=false lets both
    // sources share one 5-7 MB buffer instead of reading it twice. ImGui 1.92 requires
    // the data to outlive the atlas: function-local statics are destroyed after
    // ImGui::DestroyContext has run.
    struct FontBlob
    {
        std::vector<char> bytes;
        bool              tried = false;
        bool              ok    = false;
    };

    EditorTheme::Fonts s_fonts;

    FontBlob& Blob(const char* path)
    {
        static FontBlob s_latin, s_latinBold, s_tc, s_jp, s_sc, s_kr, s_icons, s_legacy;
        FontBlob* b = &s_legacy;
        if      (path == kNotoLatin)     b = &s_latin;
        else if (path == kNotoLatinBold) b = &s_latinBold;
        else if (path == kNotoTC)        b = &s_tc;
        else if (path == kNotoJP)        b = &s_jp;
        else if (path == kNotoSC)        b = &s_sc;
        else if (path == kNotoKR)        b = &s_kr;
        else if (path == kIconFont)      b = &s_icons;
        const bool optional = (path == kNotoSC || path == kNotoKR);
        if (!b->tried)
        {
            b->tried = true;
            // AssetFS, not a bare ifstream on the cwd: the engine root, and a
            // worktree that reads its fonts from the main checkout
            // (Resource/SharedContent.h), resolve the same way every asset does.
            std::vector<std::uint8_t> bytes;
            if (Resource::AssetFS::Get().ReadFile(path, bytes) && !bytes.empty())
            {
                b->bytes.assign(bytes.begin(), bytes.end());
                b->ok = true;
            }
            if (!b->ok && !optional)
                LOG_WARNING("EditorTheme: font file not found or unreadable: '%s'", path);
        }
        return *b;
    }

    ImFont* AddSource(const char* path, float sizePx, bool merge,
                      const ImWchar* ranges, const ImWchar* exclude,
                      const char* debugName, float minAdvanceX = 0.0f)
    {
        FontBlob& b = Blob(path);
        if (!b.ok) return nullptr;

        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.MergeMode            = merge;
        cfg.GlyphExcludeRanges   = exclude;
        cfg.GlyphMinAdvanceX     = minAdvanceX;
        cfg.PixelSnapH           = minAdvanceX > 0.0f;   // icons: whole-pixel advance
        cfg.Flags               |= ImFontFlags_NoLoadError;
        snprintf(cfg.Name, sizeof(cfg.Name), "%s", debugName);
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
            b.bytes.data(), static_cast<int>(b.bytes.size()), sizePx, &cfg, ranges);
    }

    // One complete chain: Latin face (regular or SemiBold) -> icons -> TC -> JP.
    // Returns the root ImFont, or nullptr when the Latin face is missing.
    ImFont* BuildChain(const char* latinPath, float sizePx, const char* name, bool& iconsOut)
    {
        ImFont* root = AddSource(latinPath, sizePx, /*merge*/ false, nullptr, kPuaRange, name);
        if (!root) return nullptr;
        // GlyphMinAdvanceX keeps icon columns aligned in lists (outliner,
        // asset tiles). It is an absolute value for the load size and scales
        // with the font size at runtime.
        iconsOut = AddSource(kIconFont, sizePx, true, kPuaRange, nullptr,
                             "Font Awesome 7 Solid", sizePx * 0.75f) != nullptr;
        AddSource(kNotoTC, sizePx, true, nullptr, kPuaRange, "Noto Sans TC");
        AddSource(kNotoJP, sizePx, true, nullptr, kPuaRange, "Noto Sans JP");
        AddSource(kNotoSC, sizePx, true, nullptr, kPuaRange, "Noto Sans SC");   // optional
        AddSource(kNotoKR, sizePx, true, nullptr, kPuaRange, "Noto Sans KR");   // optional
        return root;
    }
}

namespace EditorTheme
{
    void ApplyStyle()
    {
        ImGuiStyle& s = ImGui::GetStyle();

        // ---- Geometry ----
        // Paddings / spacings are LAYOUT, not decoration: FramePadding.y and the font size
        // decide the menu-bar and toolbar heights, which decide the Viewport rectangle the
        // harness packs measure. Changing them moves that rectangle.
        s.WindowRounding    = 4.0f;
        s.ChildRounding     = 3.0f;
        s.FrameRounding     = 3.0f;
        s.PopupRounding     = 4.0f;
        s.ScrollbarRounding = 6.0f;
        s.GrabRounding      = 3.0f;
        s.TabRounding       = 3.0f;
        s.WindowPadding     = ImVec2(10, 10);
        s.FramePadding      = ImVec2(6, 4);
        s.ItemSpacing       = ImVec2(8, 5);
        s.ItemInnerSpacing  = ImVec2(6, 4);
        s.IndentSpacing     = 18.0f;
        s.ScrollbarSize     = 14.0f;
        s.GrabMinSize       = 10.0f;
        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.TabBorderSize     = 0.0f;
        s.TabBarOverlineSize = 2.0f;
        s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
        s.SeparatorTextBorderSize = 1.0f;
        s.SeparatorTextPadding    = ImVec2(0.0f, 3.0f);

        // ---- Palette ----
        // Neutral charcoal surfaces, ONE accent (blue) reserved for state: the selected
        // tab's overline, checks, slider grabs, the active item and drop targets. Plain
        // buttons stay neutral so the accent can mark "this one is on".
        const ImVec4 accent       (0.26f, 0.53f, 0.93f, 1.00f);
        const ImVec4 accentHover  (0.34f, 0.60f, 0.98f, 1.00f);
        const ImVec4 accentDim    (0.20f, 0.40f, 0.70f, 1.00f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                  = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.54f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.110f, 0.110f, 0.118f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.094f, 0.094f, 0.102f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.125f, 0.125f, 0.135f, 0.98f);
        c[ImGuiCol_Border]                = ImVec4(0.200f, 0.200f, 0.220f, 1.00f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.165f, 0.165f, 0.178f, 1.00f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.205f, 0.205f, 0.222f, 1.00f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.235f, 0.235f, 0.255f, 1.00f);
        c[ImGuiCol_TitleBg]               = ImVec4(0.078f, 0.078f, 0.086f, 1.00f);
        c[ImGuiCol_TitleBgActive]         = ImVec4(0.094f, 0.094f, 0.102f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.078f, 0.078f, 0.086f, 0.75f);
        c[ImGuiCol_MenuBarBg]             = ImVec4(0.086f, 0.086f, 0.094f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.094f, 0.094f, 0.102f, 0.60f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.260f, 0.260f, 0.280f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.320f, 0.320f, 0.345f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.380f, 0.380f, 0.410f, 1.00f);
        c[ImGuiCol_CheckMark]             = accentHover;
        c[ImGuiCol_SliderGrab]            = accent;
        c[ImGuiCol_SliderGrabActive]      = accentHover;
        c[ImGuiCol_Button]                = ImVec4(0.205f, 0.205f, 0.222f, 1.00f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.270f, 0.270f, 0.292f, 1.00f);
        c[ImGuiCol_ButtonActive]          = accentDim;
        c[ImGuiCol_Header]                = ImVec4(0.190f, 0.230f, 0.310f, 1.00f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.240f, 0.300f, 0.420f, 1.00f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.260f, 0.380f, 0.580f, 1.00f);
        c[ImGuiCol_Separator]             = ImVec4(0.200f, 0.200f, 0.220f, 1.00f);
        c[ImGuiCol_SeparatorHovered]      = accentDim;
        c[ImGuiCol_SeparatorActive]       = accent;
        c[ImGuiCol_ResizeGrip]            = ImVec4(0.300f, 0.300f, 0.330f, 0.40f);
        c[ImGuiCol_ResizeGripHovered]     = accentDim;
        c[ImGuiCol_ResizeGripActive]      = accent;
        c[ImGuiCol_InputTextCursor]       = c[ImGuiCol_Text];
        c[ImGuiCol_Tab]                   = ImVec4(0.110f, 0.110f, 0.118f, 1.00f);
        c[ImGuiCol_TabHovered]            = ImVec4(0.240f, 0.300f, 0.420f, 1.00f);
        c[ImGuiCol_TabSelected]           = ImVec4(0.170f, 0.170f, 0.185f, 1.00f);
        c[ImGuiCol_TabSelectedOverline]   = accent;
        c[ImGuiCol_TabDimmed]             = ImVec4(0.086f, 0.086f, 0.094f, 1.00f);
        c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.135f, 0.135f, 0.148f, 1.00f);
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.330f, 0.330f, 0.360f, 1.00f);
        c[ImGuiCol_DockingPreview]        = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.070f, 0.070f, 0.075f, 1.00f);
        c[ImGuiCol_PlotLines]             = ImVec4(0.60f, 0.60f, 0.64f, 1.00f);
        c[ImGuiCol_PlotLinesHovered]      = accentHover;
        c[ImGuiCol_PlotHistogram]         = accent;
        c[ImGuiCol_PlotHistogramHovered]  = accentHover;
        c[ImGuiCol_TableHeaderBg]         = ImVec4(0.140f, 0.140f, 0.152f, 1.00f);
        c[ImGuiCol_TableBorderStrong]     = ImVec4(0.220f, 0.220f, 0.240f, 1.00f);
        c[ImGuiCol_TableBorderLight]      = ImVec4(0.170f, 0.170f, 0.185f, 1.00f);
        c[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
        c[ImGuiCol_TextLink]              = accentHover;
        c[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_TreeLines]             = ImVec4(0.300f, 0.300f, 0.330f, 1.00f);
        c[ImGuiCol_DragDropTarget]        = accentHover;
        c[ImGuiCol_UnsavedMarker]         = ImVec4(1.00f, 0.62f, 0.22f, 1.00f);
        c[ImGuiCol_NavCursor]             = accent;
        c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
        c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    }

    const Fonts& LoadFonts(float sizePx)
    {
        sizePx = std::clamp(sizePx, kMinFontSize, kMaxFontSize);
        ImGuiIO& io = ImGui::GetIO();
        s_fonts = {};

        bool icons = false;
        s_fonts.regular = BuildChain(kNotoLatin, sizePx, "Noto Sans", icons);
        if (s_fonts.regular)
        {
            s_fonts.noto  = true;
            s_fonts.icons = icons;
            bool boldIcons = false;
            s_fonts.bold = BuildChain(kNotoLatinBold, sizePx, "Noto Sans SemiBold", boldIcons);
        }
        else
        {
            // Fallback 1: HigashiOme Gothic (Japanese) + icons. Fallback 2: ImGui's built-in
            // ProggyClean (ASCII only) + icons.
            s_fonts.regular = AddSource(kLegacyCjk, sizePx, false, nullptr, kPuaRange, "HigashiOme Gothic");
            if (!s_fonts.regular)
            {
                ImFontConfig cfg;
                cfg.SizePixels = sizePx;
                s_fonts.regular = io.Fonts->AddFontDefault(&cfg);
            }
            s_fonts.icons = AddSource(kIconFont, sizePx, true, kPuaRange, nullptr,
                                      "Font Awesome 7 Solid", sizePx * 0.75f) != nullptr;
            LOG_WARNING("EditorTheme: Noto Sans not found under engine/editor/fonts/ - "
                        "editor text falls back to '%s'",
                        Blob(kLegacyCjk).ok ? kLegacyCjk : "ImGui default (ASCII only)");
        }
        if (!s_fonts.bold) s_fonts.bold = s_fonts.regular;

        io.FontDefault = s_fonts.regular;
        ImGui::GetStyle().FontSizeBase = sizePx;
        LOG_INFO("EditorTheme: fonts ready - face=%s size=%.0fpx icons=%d bold=%d sc=%d kr=%d",
                 s_fonts.noto ? "Noto Sans + TC + JP" : "fallback", sizePx,
                 s_fonts.icons ? 1 : 0, s_fonts.bold != s_fonts.regular ? 1 : 0,
                 Blob(kNotoSC).ok ? 1 : 0, Blob(kNotoKR).ok ? 1 : 0);
        return s_fonts;
    }

    const Fonts& GetFonts() { return s_fonts; }

    float SetFontSize(float px)
    {
        px = std::clamp(px, kMinFontSize, kMaxFontSize);
        ImGuiStyle& s = ImGui::GetStyle();
        s.FontSizeBase           = px;
        s._NextFrameFontSizeBase = px;   // ImGui 1.92: applied by the next NewFrame
        return px;
    }

    void PushBold()
    {
        ImGui::PushFont(s_fonts.bold, 0.0f);   // 0 = keep the current size
    }

    void PopBold()
    {
        ImGui::PopFont();
    }

    const char* Icon(const char* icon)
    {
        return s_fonts.icons ? icon : nullptr;
    }

    void MenuSection(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::SeparatorText(text);
        ImGui::PopStyleColor();
    }

    void Heading(const char* text)
    {
        ImGui::Spacing();
        PushBold();
        ImGui::SeparatorText(text);
        PopBold();
    }
}
