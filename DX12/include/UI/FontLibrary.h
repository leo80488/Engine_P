#pragma once

// FontLibrary - owns every runtime Font instance (one per TTF path).
//
// Any text element can carry a `fontPath` and resolve its own Font through
// Resolve(); an empty path (or a load failure) falls back to the engine default
// font (entry 0, loaded at App init from settings.json's uiDefaultFont or the
// built-in default).
//
// Every non-default font falls back to the default font for codepoints its face
// lacks (a Latin-only face still shows CJK through the default), and
// unresolvable codepoints render as tofu boxes (Font.cpp).
//
// Frame contract:
//   * GetOrLoad / Resolve / EnsureGlyph: CPU tick phase, main thread only.
//   * Find: bake-free, safe from render passes.
//   * FlushPendingUploads(): called ONCE per frame by App, after all UI CPU
//     emission and before the render graph executes; renders (or, under
//     Font::BakeMode::Next, hands off) every glyph cell this frame reserved,
//     all fonts at once, and uploads every atlas page it dirtied.
//
// Lifetime: App-scoped. Fonts and their atlas pages survive OnWorldClear; only
// Shutdown() (device teardown) frees them, keeping every SRV / bindless
// descriptor slot stable for the whole run.

#include "UI/Font.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class IGraphicsDevice;

namespace UI
{
    class FontLibrary
    {
    public:
        // Base SDF bake size for every font unless overridden per load. 32px
        // keeps large text crisp while a CJK cell (~40x40 with spread) still
        // fits ~600 glyphs per 1024^2 page.
        static constexpr float kDefaultBasePixelSize = 32.f;

        static FontLibrary& Get();

        // Create the shared FreeType library and load the default font
        // (entry 0). Call once at App init. Returns false when the default
        // TTF cannot be opened (text rendering is then disabled).
        bool Init(IGraphicsDevice& gfx, const std::string& defaultTtfPath,
                  float basePixelSize = kDefaultBasePixelSize);

        // Device-teardown ONLY: frees every Font (faces + atlas pages) and
        // the FreeType library.
        void Shutdown();

        // Load (or return the cached) font for @p ttfPath. Slash-direction is
        // normalised for keying. Returns nullptr on failure (missing file /
        // FT error); failures are negative-cached so a bad inspector path
        // doesn't re-probe the disk every frame. Main thread only.
        Font* GetOrLoad(const std::string& ttfPath, float basePixelSize = 0.f);

        // Component-path convenience: empty path or load failure -> Default().
        Font& Resolve(const std::string& ttfPath);

        // Bake-free lookup for render passes: never loads, returns nullptr if
        // the font was never loaded this run.
        Font* Find(const std::string& ttfPath) const;

        // The engine default font (entry 0). Only valid after Init succeeded;
        // IsReady() gates callers that may run before/without a font.
        Font& Default();
        bool  IsReady() const { return !m_fonts.empty() && m_fonts[0]->IsReady(); }

        // Render the cells every font reserved this frame - one
        // Font::RenderPending over all of them, or under BakeMode::Next finish
        // the previous hand-off and hand this frame's off - then stream each
        // dirty page's dirty rect. Once per frame, before the graph.
        void FlushPendingUploads();

        // Replace @p font's fallback chain (Init wires non-default fonts to
        // fall back to the default automatically).
        void SetFallbackChain(Font& font, std::vector<Font*> chain);

        // One [FONT_BAKE] summary line: the bake totals since boot
        // (UI/GlyphStats.h) and every open font's atlas. The harness prints it
        // at the end of a run (HarnessCaptureSystem::OnUnregister).
        void LogBakeSummary() const;

        // Finish the next-frame render the last flush handed off and write its
        // cells into the atlas (they upload at the next flush).
        // FlushPendingUploads calls it under -glyphbake=next; the harness at
        // the end of a run, so the summary counts every cell. Main thread.
        void FinishAsyncRender();

        // Editor introspection.
        const std::vector<std::unique_ptr<Font>>& Fonts() const { return m_fonts; }

    private:
        FontLibrary() = default;
        ~FontLibrary() = default;
        FontLibrary(const FontLibrary&) = delete;
        FontLibrary& operator=(const FontLibrary&) = delete;

        static std::string NormalizeKey(const std::string& path);
        void LowLoadArm();   // -fonttest_lowload (harness): Low-queue load ahead of a hand-off

        IGraphicsDevice* m_gfx = nullptr;
        FT_Library       m_ft  = nullptr;
        float            m_basePixelSize = kDefaultBasePixelSize;

        // The one next-frame render in flight (the previous flush's cells) and
        // how many Low-priority tasks render a flush's cells.
        std::shared_ptr<Font::AsyncRender> m_async;
        uint32_t                           m_asyncTasks = 1;

        std::vector<std::unique_ptr<Font>> m_fonts;   // [0] = default
        std::unordered_set<std::string>    m_failed;  // negative load cache
    };

} // namespace UI
