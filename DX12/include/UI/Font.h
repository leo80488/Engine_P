#pragma once

// Font - FreeType-backed SDF text rendering with on-demand glyph baking.
//
// A Font keeps its FreeType face open for its whole lifetime and bakes a glyph
// the first time it is needed (EnsureGlyph / DrawText / Measure): FreeType SDF
// (8SSEDT as fallback), shelf-packed into a CPU-side R8 atlas page. A full
// 1024x1024 page allocates a new one: no fixed glyph budget and no pre-baked
// range (Init prewarms ASCII only to avoid first-frame hitches).
//
// Reserve first, render at the flush: a request packs the glyph's cell and
// publishes its final FontGlyph (UVs, size, bearing, advance, all known from the
// outline's control box, UI/GlyphGeometry.h), so this frame's layout, wrapping
// and world-text vertices read the real metrics. FontLibrary::FlushPendingUploads
// renders every font's reserved cells once a frame (see BakeMode), integrates
// them in reservation order and only then uploads. The atlas is byte for byte
// what rendering at the request gives (font_glyph_flood pins it).
//
// GPU pages are created once and never destroyed before Shutdown, so their SRV /
// bindless slots stay stable for the app lifetime (a UI image can never alias the
// atlas's descriptor). A baked glyph only grows its page's dirty rect;
// FlushPendingUploads, which the App calls once per frame after all UI CPU
// emission and before the hand-off to the render half, streams that rect
// (IGraphicsDevice::QueueTextureUpload, MustDeliver): recorded first in this
// frame's render half, ahead of every UI pass, with no GPU wait. Pages are
// created without initial data (all zeros on both sides). -font_syncupload
// instead uploads whole pages through UpdateTexture with a CPU wait per flush
// (pages created with their data), for A/B.
// Render passes must use the bake-free lookups (FindGlyph / MeasureResident),
// never EnsureGlyph / DrawText / Measure.
//
// Missing codepoints walk the fallback chain (FontLibrary); if no font in the
// chain covers them a synthetic "tofu" box is baked so the text still advances.
//
// Fonts are owned by FontLibrary (UI/FontLibrary.h), one per TTF path. Per-call
// size and effects travel in TextDrawParams; there is no global scale state.

#include "UI/UIDrawList.h"
#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;

// FreeType forward declarations: they mirror ft2build's own typedefs so the
// FreeType headers stay out of the engine include graph.
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_*    FT_Face;

namespace UI
{
    struct FontGlyph
    {
        Vec2     uv0{};       // top-left in [0,1] of the owning atlas page
        Vec2     uv1{};       // bottom-right in [0,1]
        Vec2     size{};      // cell size in baked pixels (INCLUDES 2*spread pad)
        Vec2     bearing{};   // baked-pixel offset from pen-on-baseline to cell top-left
        float    advance = 0.f; // baked-pixel advance to the next pen position
        uint16_t page    = 0;   // atlas page index within the owning Font
    };

    struct FontMetrics
    {
        float pixelSize  = 32.f; // base SDF bake size (glyph metrics are in these px)
        float lineHeight = 37.f; // line advance in baked pixels
        float ascender   = 28.f; // baseline -> top of glyph (positive, baked px)
        float descender  = 9.f;  // baseline -> bottom of glyph (positive, baked px)
        // SDF range: the glyph-space pixel span mapped across the [0,1]
        // distance field (== 2 * kSdfSpread). Effect widths in pixels are
        // normalised by this for the shader.
        float sdfPixelRange = 8.f;
    };

    // Codepoint range [first..last] inclusive. 32..126 is printable ASCII.
    struct CodepointRange { uint32_t first; uint32_t last; };

    // Reference on-screen pixel size for the `fontScale` / `scale` component
    // fields: fontScale 1.0 == kTextRefPx px tall text, whatever the SDF base
    // bake size, so scenes keep the apparent text size they were authored at.
    constexpr float kTextRefPx = 24.f;

    // Per-call draw parameters (there is no global scale state).
    struct TextDrawParams
    {
        float      sizePx  = kTextRefPx; // on-screen pixel size (em height)
        TextEffect effects{};
        float      timeSec = 0.f;        // drives jitter animation
    };

    // Shared UTF-8 decoder: returns the codepoint and advances p past the
    // consumed bytes (1-4). Malformed input yields U+FFFD and advances 1 byte.
    uint32_t DecodeUtf8(const char*& p);

    class Font
    {
    public:
        // SDF spread (glyph-space pixels of distance stored on each side of
        // the edge). Cells are padded by this so outline/glow have room.
        static constexpr int      kSdfSpread = 4;
        // Atlas page side in texels. R8_UNORM: 1 MB per page.
        static constexpr uint32_t kPageSize  = 1024;

        // Use FreeType's SDF renderers (FT_RENDER_MODE_SDF, FT 2.11+): sub-pixel
        // distances, smooth edges at any draw scale. Cleared when the sdf module
        // is missing (FontLibrary::Init), or by the first failed SDF render under
        // the harness reverse arm -fonttest_globalflip; then every glyph falls
        // back to coverage + 8SSEDT (contours quantised to whole pixels, visibly
        // lumpier under scaling). Otherwise a glyph whose own SDF render fails
        // takes that fallback alone (BakeGlyphNow, IntegrateCells).
        static bool s_useFtSdf;

        enum class SdfSource : uint8_t { Bsdf, Outline };
        static SdfSource s_sdfSource;

        enum class BakeMode : uint8_t { Parallel, Serial, Next };
        static BakeMode s_bakeMode;

        // A glyph lookup result. `owner` is the Font whose atlas pages hold
        // the glyph (== this font, or a fallback for codepoints this face
        // lacks). All FontGlyph metrics are in the OWNER's baked pixel units:
        // scale them by (sizePx / owner->Metrics().pixelSize) when drawing.
        struct ResolvedGlyph
        {
            const FontGlyph* glyph = nullptr;
            const Font*      owner = nullptr;
        };

        struct AsyncRender;

        struct AtlasHash { uint64_t combined = 0, layout = 0, pixels = 0; };

        // Which of FreeType's SDF renderers draws a cell (-glyphsdf=, read by
        // FontLibrary::Init). Bsdf, the default: the coverage render, then the
        // SDF renderer on that bitmap - 2.5x cheaper than measuring the outline,
        // the same cell, edges within 0.04 px of it on average. Outline: the
        // distance measured from the outline itself.
        static const char* SdfSourceName(SdfSource s);

        // How an on-demand glyph's pixels are rendered (-glyphbake=, read by
        // FontLibrary::Init). Every mode reserves the cell at the request.
        // Parallel: rendered at the flush on the TaskSystem's workers, drawn in
        // the frame that asked. Serial: rendered at the flush on the main thread.
        // Next: handed at the flush to a few Low-priority tasks that render while
        // the frame goes on, finished by the NEXT flush - drawn one frame late,
        // its cell blank (all zero) for that frame. Default: Parallel for a
        // harness run (-artifacts, -unattended), Next for any other run. In every
        // mode a font whose prewarm contradicts the predicted cells, a missing
        // sdf module, or a glyph that is not an outline renders at the request.
        static const char* BakeModeName(BakeMode m);

        // Open @p ttfPath (via AssetFS, so packed builds read game.ipak) at
        // @p basePixelSize and prewarm printable ASCII. The face stays open
        // until Shutdown. Returns false if the TTF is missing/corrupt.
        bool Init(IGraphicsDevice& gfx, FT_Library ft,
                  const std::string& ttfPath, float basePixelSize);

        // Device teardown only (FontLibrary::Shutdown). Frees the face and
        // destroys the atlas pages; never call it mid-frame.
        void Shutdown();

        // ---- Glyph residency (CPU tick phase only: bakes on miss) ----
        // Look up @p codepoint, baking it now if absent: own face first, then
        // the fallback chain, then a synthetic tofu box. Main thread only.
        ResolvedGlyph EnsureGlyph(uint32_t codepoint);
        // Bake every codepoint of @p utf8 that isn't resident yet.
        void EnsureGlyphs(const char* utf8);
        // Prewarm an inclusive range; codepoints the face lacks are skipped
        // (no tofu boxes: use this for ASCII/CJK warmup, not for lookup). Each
        // glyph goes through BakeGlyph like any request: when the font reserves
        // first, the cells are rendered at the next FontLibrary::FlushPendingUploads.
        void EnsureRange(uint32_t first, uint32_t last);
        // Renders the range NOW, spread over the TaskSystem's workers (each on
        // its own FreeType face), and packs the atlas here in codepoint order -
        // byte-identical to rendering one glyph at a time. Used by Init's ASCII
        // prewarm, whose [FONT] prewarm hash reads the pixels right after it.
        void PrewarmRange(uint32_t first, uint32_t last);

        // ---- Bake-free lookup (safe from render passes / worker threads) ----
        // Returns {nullptr,nullptr} when the glyph was never baked. Checks
        // this font's table first, then the fallback chain's tables.
        ResolvedGlyph FindGlyph(uint32_t codepoint) const;

        // ---- Draw / measure ----
        // Emit SDF glyph quads into @p out. Bakes missing glyphs first (CPU
        // tick phase only). Returns the end pen position.
        Vec2 DrawText(UIDrawList& out, const Vec2& pos, Color32 col,
                      const char* utf8, const TextDrawParams& p);
        // Pixel-space text size at @p sizePx. Bakes on miss (CPU tick only).
        Vec2 Measure(const char* utf8, float sizePx);
        // Bake-free Measure over already-resident glyphs (render-pass safe).
        Vec2 MeasureResident(const char* utf8, float sizePx) const;

        // ---- Word wrap ----
        // Returns @p utf8 with '\n' inserted so no line exceeds @p maxWidthPx at
        // @p sizePx; existing newlines stay hard breaks. A string transform rather
        // than a wrap-aware draw on purpose: DrawText, Measure and MeasureResident
        // already treat '\n' identically, and a fourth width-tracking loop could
        // disagree with them (text measured at one size and drawn at another).
        // Breaks after a space and between CJK codepoints (CJK has no spaces),
        // never before common closing punctuation (U+3002 ideographic full stop,
        // U+FF09 fullwidth right parenthesis, ...). A run with no break
        // opportunity (a long URL, a German compound) breaks mid-run rather than
        // overflowing.
        // CPU tick phase only: bakes any missing glyph to know its advance.
        std::string WrapText(const char* utf8, float sizePx, float maxWidthPx);

        float LineHeight(float sizePx) const
        { return m_metrics.lineHeight * (sizePx / m_metrics.pixelSize); }
        float Ascender(float sizePx) const
        { return m_metrics.ascender   * (sizePx / m_metrics.pixelSize); }

        // ---- Atlas pages ----
        uint32_t PageCount() const { return static_cast<uint32_t>(m_pages.size()); }
        // SRV GPU handle for UIPass (t3) / ImGui preview. 0 when out of range.
        uint64_t PageSrvHandle(uint32_t page) const;
        // Bindless index into the engine's t0 space2 table (WorldUI pass).
        // Returns ~0u when out of range.
        uint32_t PageBindlessIndex(uint32_t page) const;
        // Stream every dirty page's dirty rect (whole pages through UpdateTexture
        // under -font_syncupload). Called once per frame by
        // FontLibrary::FlushPendingUploads, which has rendered every font's
        // reserved cells first, and by Init after the prewarm. Reserved cells
        // still pending here (this font's alone) are rendered first.
        void FlushPendingUploads();
        // Dirty pages or reserved cells not rendered yet.
        bool HasPendingUpload() const;

        // Render the reserved cells of @p fonts - one ParallelFor over all of
        // them (on this thread under -glyphbake=serial, for a single cell, or
        // with fewer than two workers) - and write them into their pages in
        // reservation order. Main thread, game half; called by
        // FontLibrary::FlushPendingUploads before the uploads.
        static void RenderPending(Font* const* fonts, size_t count);

        // The next-frame contract (BakeMode::Next). StartAsync takes the
        // reserved cells of @p fonts and hands them to @p tasks Low-priority
        // TaskSystem tasks, which claim cells one at a time and render them
        // while the frame goes on; nothing waits. FinishAsync, at the NEXT
        // flush, renders every cell no task has claimed yet (on all workers, as
        // RenderPending), waits only for the cells a task is still rendering -
        // one per task at most - and writes them all into their pages: late by
        // exactly one flush. Main thread, game half; FontLibrary owns the one
        // outstanding render.
        static std::shared_ptr<AsyncRender> StartAsync(Font* const* fonts, size_t count, uint32_t tasks);
        static void FinishAsync(AsyncRender& render);

        // FNV-1a hashes of the CPU side of the atlas: `pixels` over every page's
        // mirror, `layout` over the glyph table in codepoint order (UVs, size,
        // bearing, advance, page), and `combined`, both in one stream - the
        // [FONT] prewarm line's hash=. Reads ~1 MB per page: call it when asked,
        // not per frame.
        AtlasHash HashAtlas() const;
        // Cells reserved whose pixels are not in the atlas yet - waiting for
        // the flush, or handed to a next-frame render; the pixel hash describes
        // the final atlas only while this is 0.
        uint32_t PendingCells() const { return static_cast<uint32_t>(m_pending.size()) + m_inflight; }
        // ...of which reserved and not handed to any render yet.
        uint32_t ReservedCells() const { return static_cast<uint32_t>(m_pending.size()); }

        // ---- Fallbacks (managed by FontLibrary::SetFallbackChain) ----
        void SetFallbacks(std::vector<Font*> chain) { m_fallback = std::move(chain); }

        // ---- Accessors ----
        const FontMetrics& Metrics() const { return m_metrics; }
        const std::string& TTFPath() const { return m_ttfPath; }
        const std::unordered_map<uint32_t, FontGlyph>& Glyphs() const { return m_glyphs; }
        bool IsReady() const { return m_face != nullptr && !m_pages.empty(); }

    private:
        struct AtlasPage
        {
            RHI::Texture         tex{};
            uint64_t             srv = 0;
            std::vector<uint8_t> pixels;        // kPageSize x kPageSize R8, persistent CPU mirror
            uint32_t             penX = 1, penY = 1, rowH = 0; // shelf packer state
            bool                 dirty = false; // CPU pixels newer than GPU
            // What WriteCell wrote since the last upload: [x0, x1) x [y0, y1).
            uint32_t             dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
        };

        // A cell packed and published (its FontGlyph is final) whose pixels
        // are not rendered yet. Kept in reservation order.
        struct PendingCell
        {
            uint32_t cp = 0;
            uint32_t page = 0, x = 0, y = 0;   // where in the atlas
            int      w = 0, h = 0;             // the predicted cell, spread included
            int      left = 0, top = 0;        // the predicted bitmap_left / bitmap_top
            bool     forceFail = false;        // -fonttest_sdffail names it (harness)
        };
        struct RenderJob;   // Font.cpp: one cell's render, on whichever thread

        bool             AddPage();                 // CreateTexture once; never destroyed before Shutdown
        bool             FlushStreamed(AtlasPage& pg);   // the dirty rect, queued (QueueTextureUpload)
        void             FlushSynchronous();             // -font_syncupload: whole pages, waited for
        bool             PackCell(uint32_t cellW, uint32_t cellH,
                                  uint32_t& outPage, uint32_t& outX, uint32_t& outY);
        // Face MUST contain cp. Reserves when ReserveFirst(), else renders
        // now (BakeGlyphNow).
        const FontGlyph* BakeGlyph(uint32_t cp);
        const FontGlyph* BakeGlyphNow(uint32_t cp); // renders at the request
        const FontGlyph* ReserveGlyph(uint32_t cp); // the cell now, the pixels at the flush
        bool             ReserveFirst() const;
        bool             PredictionsHold();         // Init: the prewarm's cells == the predicted ones
        const FontGlyph* BakeTofu(uint32_t cp);     // synthetic missing-glyph box
        const FontGlyph* StoreCell(uint32_t cp, const std::vector<uint8_t>& sdf,
                                   uint32_t cellW, uint32_t cellH,
                                   float bearingX, float bearingY, float advance);
        // Copy a w x h cell into page @p page at (x, y) of the CPU mirror and
        // grow that page's dirty rect over it.
        void             WriteCell(uint32_t page, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   const uint8_t* src);
        static FontGlyph CellGlyph(uint32_t page, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   float bearingX, float bearingY, float advance);
        // The main-thread half of a render, for @p n of this font's cells in
        // reservation order: what no worker could render (retried, else
        // 8SSEDT), then every cell into its page.
        void             IntegrateCells(const PendingCell* cells, RenderJob* jobs, size_t n);
        // FreeType's SDF of one job (RenderSdf) on @p face, the calling
        // thread's own.
        static void      RenderCell(FT_Face face, RenderJob& job);
        // Next-frame render: claim cells of @p render and render them until
        // none is left, on a FreeType library and faces of this call's own.
        // Returns how many this call rendered.
        static uint32_t  DrainAsync(AsyncRender& render);
        bool             HasCodepoint(uint32_t cp) const;
        void             AssertMainThread() const;

        IGraphicsDevice*      m_gfx  = nullptr;
        FT_Face               m_face = nullptr;
        std::vector<uint8_t>  m_ttfBytes;  // FT memory-face requires the bytes stay alive
        std::string           m_ttfPath;
        FontMetrics           m_metrics;
        std::thread::id       m_mainThread{};

        std::unordered_map<uint32_t, FontGlyph> m_glyphs;
        std::vector<AtlasPage>                  m_pages;
        std::vector<Font*>                      m_fallback;
        std::vector<uint32_t>                   m_sdfFailures; // SDF render failed: warned once each (BakeGlyphNow, IntegrateCells)
        // BakeTofu's box SDF and the box it was computed for: every box of a
        // font is that one cell, so later boxes copy it. Main thread only.
        std::vector<uint8_t>                    m_tofuSdf;
        int                                     m_tofuBoxW = 0, m_tofuBoxH = 0, m_tofuStroke = 0;
        // Reserved cells. Only the main thread touches these (render workers
        // get copies of what they read: the TTF bytes' address, the pixel size,
        // the codepoint).
        std::vector<PendingCell>                m_pending;
        uint32_t                                m_inflight = 0;   // this font's cells in a next-frame render
        bool                                    m_reserveFirst = false;   // PredictionsHold at Init
        uint32_t                                m_mismatchLogged = 0;
    };

    // Shim for the engine default font: forwards to
    // FontLibrary::Get().Default(). Prefer FontLibrary::Resolve(fontPath).
    Font& DefaultFont();

} // namespace UI
