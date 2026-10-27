#pragma once
// EditorTheme.h - the editor's look: ImGui style (geometry + palette) and the
// typography chain.
//
// TYPOGRAPHY. Every face below is a MERGED chain, so every piece of editor text
// renders Latin, Traditional/Simplified Chinese, Japanese and Font Awesome icons
// from the same ImFont:
//
//     Noto Sans (Latin/Greek/Cyrillic)
//       -> Font Awesome 7 Solid, the WHOLE Private Use Area (U+E000..U+F8FF)
//       -> Noto Sans TC (Han with Taiwanese glyph shapes, Bopomofo, kana)
//       -> Noto Sans JP (Japanese-only kanji / shapes the TC face lacks)
//       -> Noto Sans SC, Noto Sans KR - OPTIONAL: picked up when their folders
//          exist under engine/editor/fonts/ (TC + JP leave some Simplified-only
//          hanzi and all Hangul uncovered), silently skipped otherwise
//
// ImGui 1.92's atlas is dynamic: a glyph is rasterised the first time it is drawn,
// so the ~20k-glyph CJK faces cost nothing up front and there is no glyph-range
// list to maintain (every Font Awesome codepoint renders). The Latin and CJK
// sources EXCLUDE the PUA so nothing can shadow an icon.
//
// The Noto files live under engine/editor/fonts/, which tools/package_game.py
// never packs: they are Editor.exe-only. Missing files degrade (HigashiOme Gothic,
// then ImGui's built-in font) with a log line, never a crash.

struct ImFont;

namespace EditorTheme
{
    inline constexpr float kDefaultFontSize = 16.0f;   // px, at 100% UI scale
    inline constexpr float kMinFontSize     = 12.0f;
    inline constexpr float kMaxFontSize     = 24.0f;

    struct Fonts
    {
        ImFont* regular      = nullptr;   // io.FontDefault: body text everywhere
        ImFont* bold         = nullptr;   // headings; == regular when SemiBold is missing
        bool    noto         = false;     // false: a fallback face is in use
        bool    icons        = false;     // Font Awesome merged
    };

    // Geometry + dark palette. Idempotent.
    void ApplyStyle();

    // Build the font chain. Call ONCE, before the first NewFrame (the atlas is
    // handed to the renderer backend on its first frame).
    const Fonts& LoadFonts(float sizePx);

    // What LoadFonts produced (all null before it ran).
    const Fonts& GetFonts();

    // Runtime font size (ImGui 1.92: style.FontSizeBase, applied next frame).
    // Clamped to [kMinFontSize, kMaxFontSize]; returns the value applied.
    float SetFontSize(float px);

    // A section heading in the bold face with a separator line after it: the look
    // every settings page uses for its groups.
    void Heading(const char* text);

    // Push/pop the bold face at the current size (no-op pair if not loaded).
    void PushBold();
    void PopBold();

    // `icon` when Font Awesome is merged, else nullptr: for the ICON COLUMN of
    // ImGui::MenuItemEx / BeginMenuEx (imgui_internal.h). The column keeps every label
    // in a menu aligned whether or not its item has an icon, and the item's ID stays
    // the plain label.
    const char* Icon(const char* icon);

    // A group heading inside a menu or popup: SeparatorText in the dimmed text
    // colour, so it reads as structure rather than as a clickable item.
    void MenuSection(const char* text);
}
