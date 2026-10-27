#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "UI/GlyphGeometry.h"    // the cell a glyph will need, before rendering it
#include "UI/GlyphStats.h"       // the bake's totals (frames.jsonl, Profiler.Glyphs)
#include "Diagnostics/Profiler.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"
#include "System/Check.h"
#include "System/ThreadContext.h"
#include "System/TaskSystem.h"   // PrewarmRange and RenderPending render in parallel
#include "System/CommandLine.h"  // -font_prewarm=serial

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H   // FT_Property_Set: a worker's own library takes the sdf / bsdf spread
#include FT_OUTLINE_H  // FT_Outline_Get_CBox: GlyphDrawsNothing, the reserved cell

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>   // std::strtoul: -fonttest_sdffail
#include <cstring>
#include <vector>

// ==== CPU signed-distance-field generation (8SSEDT) ====
// File-local: helper structs live in an anonymous namespace per the project's
// ODR rule (same-name structs across TUs are silent ICF merges).
namespace
{
    struct EdtPoint { int dx, dy; };
    inline int EdtDist2(const EdtPoint& p) { return p.dx * p.dx + p.dy * p.dy; }

    // 8-points Signed Sequential Euclidean Distance Transform (Danielsson).
    // Propagates, per cell, the offset vector to the nearest seeded cell.
    struct EdtGrid
    {
        int w = 0, h = 0;
        std::vector<EdtPoint> g;

        EdtPoint Get(int x, int y) const
        {
            if (x >= 0 && y >= 0 && x < w && y < h) return g[static_cast<size_t>(y) * w + x];
            return { 9999, 9999 };
        }
        void Put(int x, int y, const EdtPoint& p) { g[static_cast<size_t>(y) * w + x] = p; }
        void Compare(EdtPoint& p, int x, int y, int ox, int oy) const
        {
            EdtPoint o = Get(x + ox, y + oy);
            o.dx += ox; o.dy += oy;
            if (EdtDist2(o) < EdtDist2(p)) p = o;
        }
        void Generate()
        {
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,-1,0); Compare(p,x,y,0,-1); Compare(p,x,y,-1,-1); Compare(p,x,y,1,-1); Put(x,y,p); }
                for (int x = w - 1; x >= 0; --x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,1,0); Put(x,y,p); }
            }
            for (int y = h - 1; y >= 0; --y)
            {
                for (int x = w - 1; x >= 0; --x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,1,0); Compare(p,x,y,0,1); Compare(p,x,y,-1,1); Compare(p,x,y,1,1); Put(x,y,p); }
                for (int x = 0; x < w; ++x)
                { EdtPoint p = Get(x, y); Compare(p,x,y,-1,0); Put(x,y,p); }
            }
        }
    };

    // Build a padded (cellW x cellH) single-channel SDF (bytes, edge=0.5)
    // from an FT_PIXEL_MODE_GRAY coverage bitmap. The glyph sits `spread` px in
    // from every cell edge. `pitch` is FreeType's signed row stride (add pitch
    // to step down one logical row; valid for both up- and down-flow bitmaps).
    void ComputeGlyphSDF(const uint8_t* cover, int gw, int gh, int pitch,
                         int spread, int cellW, int cellH,
                         std::vector<uint8_t>& outAlpha)
    {
        const int N = cellW * cellH;
        EdtGrid gIn, gOut;                  // gIn seeds INSIDE cells, gOut OUTSIDE
        gIn.w = gOut.w = cellW; gIn.h = gOut.h = cellH;
        const EdtPoint kZero{ 0, 0 }, kInf{ 9999, 9999 };
        gIn.g.assign(N, kInf);
        gOut.g.assign(N, kInf);

        for (int y = 0; y < cellH; ++y)
            for (int x = 0; x < cellW; ++x)
            {
                const int gx = x - spread, gy = y - spread;
                bool inside = false;
                if (gx >= 0 && gy >= 0 && gx < gw && gy < gh)
                    inside = cover[static_cast<ptrdiff_t>(gy) * pitch + gx] >= 128;
                const size_t i = static_cast<size_t>(y) * cellW + x;
                if (inside) gIn.g[i]  = kZero;  // distance-to-inside seed
                else        gOut.g[i] = kZero;  // distance-to-outside seed
            }

        gIn.Generate();
        gOut.Generate();

        outAlpha.assign(static_cast<size_t>(N), 0);
        const float range = 2.0f * static_cast<float>(spread);
        for (int i = 0; i < N; ++i)
        {
            const float dToInside  = std::sqrt(static_cast<float>(EdtDist2(gIn.g[i])));
            const float dToOutside = std::sqrt(static_cast<float>(EdtDist2(gOut.g[i])));
            // Signed distance, positive INSIDE the glyph; edge maps to 0.5.
            const float signedDist = (EdtDist2(gIn.g[i]) == 0) ? +dToOutside : -dToInside;
            float a = 0.5f + signedDist / range;
            a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
            outAlpha[i] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }

    constexpr uint32_t kPad = 1; // padding between atlas cells to avoid bleed

    // -font_syncupload: a page created with its data, dirty pages pushed whole
    // through UpdateTexture and waited for. Kept for A/B and as
    // font_glyph_flood's reverse arm.
    bool FontSyncUpload()
    {
        static const bool s_sync = Sys::CommandLine::Get().Has("-font_syncupload");
        return s_sync;
    }

#ifdef WITH_HARNESS
    // -fonttest_globalflip (harness; the reverse arm of
    // tests/unit/test_font_sdf_fallback.py): the global SDF failure policy. A
    // zero-area outline goes to the SDF render like any other, and an SDF
    // render that leaves no bitmap switches every later glyph of every font
    // to 8SSEDT for the rest of the process.
    bool GlobalFlipArm()
    {
        static const bool s_on = Sys::CommandLine::Get().Has("-fonttest_globalflip");
        return s_on;
    }

    // -fonttest_sdffail=<ttf>:U+XXXX (harness; the same test): that font's SDF
    // render of that codepoint counts as failed - in BakeGlyphNow, in a reserved
    // cell's render (PendingCell::forceFail), and in the prewarm workers, which
    // leave it to BakeGlyphNow - so the per-glyph 8SSEDT fallback runs on a glyph
    // FreeType can render (no engine font has one that really fails).
    // BakeGlyphNow throws a SUCCESSFUL render away, so the fallback has to load
    // the outline again.
    bool SdfFailArm(const std::string& ttf, uint32_t cp)
    {
        struct Spec { std::string ttf; uint32_t cp = 0; };
        static const Spec s_spec = []
        {
            Spec s;
            const std::string arg = Sys::CommandLine::Get().Get("-fonttest_sdffail");
            const size_t colon = arg.rfind(':');
            if (colon != std::string::npos && arg.compare(colon + 1, 2, "U+") == 0)
            {
                s.ttf = arg.substr(0, colon);
                s.cp  = static_cast<uint32_t>(std::strtoul(arg.c_str() + colon + 3, nullptr, 16));
            }
            if (!arg.empty() && s.cp == 0)
                LOG_WARNING("Font: -fonttest_sdffail=%s is not <ttf>:U+XXXX - nothing fails", arg.c_str());
            return s;
        }();
        return s_spec.cp != 0 && cp == s_spec.cp && ttf == s_spec.ttf;
    }
#else
    constexpr bool GlobalFlipArm() { return false; }
    bool SdfFailArm(const std::string&, uint32_t) { return false; }
#endif

    // The SDF cell for the outline in @p g, predicted before rendering it
    // (UI/GlyphGeometry.h). Empty when the box rounds to no pixel.
    UI::GlyphGeometry::Cell PredictCell(const FT_GlyphSlot g)
    {
        FT_BBox box;
        FT_Outline_Get_CBox(&g->outline, &box);
        return UI::GlyphGeometry::FromControlBox(box.xMin, box.yMin, box.xMax, box.yMax, UI::Font::kSdfSpread);
    }

    // A glyph with nothing to draw: an outline with no contours (a space), an
    // embedded bitmap with no pixels, or an outline whose control box rounds
    // out to zero pixels in either axis. That rounding is FreeType's own
    // bitmap preset (ft_glyphslot_preset_bitmap: floor the min, ceil the max),
    // so - like the empty bitmap for bsdf - it is exactly the case in which
    // FT_Render_Glyph(FT_RENDER_MODE_SDF) succeeds and leaves NO bitmap. Real
    // fonts have them: arial.ttf's U+2000-U+200A and U+202F (thin space,
    // narrow no-break space and the other typographic spaces) are one contour
    // of one point. Each is an advance and no cell.
    bool GlyphDrawsNothing(const FT_GlyphSlot g)
    {
        if (g->format == FT_GLYPH_FORMAT_OUTLINE && g->outline.n_contours == 0) return true;
        if (GlobalFlipArm()) return false;   // the global-flip arm counts only contour-less outlines
        if (g->format == FT_GLYPH_FORMAT_BITMAP) return g->bitmap.width == 0 || g->bitmap.rows == 0;
        if (g->format != FT_GLYPH_FORMAT_OUTLINE) return false;
        return PredictCell(g).Empty();
    }

    constexpr FT_Int32 kGlyphLoad = FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING;   // see BakeGlyphNow

    // A worker's own FreeType library (PrewarmRange, RenderPending, the
    // next-frame tasks) takes both SDF renderers' spread, as FontLibrary::Init
    // sets them on the main one: FreeType's default is 8, the atlas convention 4.
    void SetSdfSpread(FT_Library ft)
    {
        FT_Int spread = UI::Font::kSdfSpread;
        FT_Property_Set(ft, "sdf", "spread", &spread);
        FT_Property_Set(ft, "bsdf", "spread", &spread);
    }

    // The distance field of the glyph in @p g's slot, in place, by
    // Font::s_sdfSource: bsdf - FreeType's coverage render, then its SDF
    // renderer on that bitmap - or the SDF measured from the outline. The cell
    // is the same either way (tests/unit/glyph_metrics_test.cpp holds both to
    // the prediction); a slot that already holds a bitmap (an embedded
    // strike) goes to bsdf in both.
    FT_Error RenderSdf(FT_GlyphSlot g)
    {
        if (UI::Font::s_sdfSource == UI::Font::SdfSource::Bsdf && g->format == FT_GLYPH_FORMAT_OUTLINE)
            if (const FT_Error e = FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL)) return e;
        return FT_Render_Glyph(g, FT_RENDER_MODE_SDF);
    }

    // ==== Bake telemetry (UI/GlyphStats.h) ====
    using BakeClock = std::chrono::steady_clock;

    // The main thread's baking time, counted ONCE however the calls nest: a
    // prewarm's leftovers go through BakeGlyphNow, and a font's EnsureGlyph bakes
    // into a fallback font's atlas.
    thread_local int t_bakeDepth = 0;
    struct MainBakeTime
    {
        BakeClock::time_point t0{};
        MainBakeTime() { if (t_bakeDepth++ == 0) t0 = BakeClock::now(); }
        ~MainBakeTime()
        {
            if (--t_bakeDepth == 0)
                UI::GlyphStats::mainNs.fetch_add(UI::GlyphStats::ToNs(BakeClock::now() - t0),
                                                 std::memory_order_relaxed);
        }
        MainBakeTime(const MainBakeTime&) = delete;
        MainBakeTime& operator=(const MainBakeTime&) = delete;
    };

    // One cell's render (FreeType's raster, or the 8SSEDT transform) has just
    // finished on this thread. @p glyph: a real glyph's cell (rendered), not
    // a tofu box (counted by BakeTofu).
    void NoteRender(BakeClock::time_point t0, bool glyph)
    {
        const uint64_t ns = UI::GlyphStats::ToNs(BakeClock::now() - t0);
        UI::GlyphStats::renderNs.fetch_add(ns, std::memory_order_relaxed);
        UI::GlyphStats::NoteMax(UI::GlyphStats::renderMaxNs, ns);
        const bool worker = TaskSystem::OnWorkerThread();
        if (worker) UI::GlyphStats::workerNs.fetch_add(ns, std::memory_order_relaxed);
        if (!glyph) return;
        UI::GlyphStats::rendered.fetch_add(1, std::memory_order_relaxed);
        if (worker) UI::GlyphStats::renderedOffMain.fetch_add(1, std::memory_order_relaxed);
    }

    void NoteEmpty() { UI::GlyphStats::empty.fetch_add(1, std::memory_order_relaxed); }

    // Copy FreeType's bitmap out of the slot, rows packed (the cell layout).
    void CopyBitmap(const FT_GlyphSlot g, std::vector<uint8_t>& out)
    {
        const uint32_t w = g->bitmap.width, h = g->bitmap.rows;
        out.resize(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * w,
                        g->bitmap.buffer + static_cast<ptrdiff_t>(y) * g->bitmap.pitch, w);
    }
} // anonymous namespace

namespace UI
{
    // ==== UTF-8 decode (shared) ====
    uint32_t DecodeUtf8(const char*& p)
    {
        const auto b0 = static_cast<uint8_t>(*p);
        if (b0 < 0x80u) { ++p; return b0; }
        auto take = [&](int extra) -> uint32_t {
            uint32_t cp = b0 & static_cast<uint8_t>(0xFFu >> (extra + 1));
            for (int i = 0; i < extra; ++i)
            {
                ++p;
                if ((static_cast<uint8_t>(*p) & 0xC0u) != 0x80u) { return 0xFFFDu; }
                cp = (cp << 6) | (static_cast<uint8_t>(*p) & 0x3Fu);
            }
            ++p;
            return cp;
        };
        if ((b0 & 0xE0u) == 0xC0u) return take(1);
        if ((b0 & 0xF0u) == 0xE0u) return take(2);
        if ((b0 & 0xF8u) == 0xF0u) return take(3);
        ++p;
        return 0xFFFDu;
    }

    // ==== Color helpers ====
    static void Color32ToFloat4(Color32 c, float out[4])
    {
        out[0] = ((c.rgba >> 0)  & 0xFFu) / 255.f;
        out[1] = ((c.rgba >> 8)  & 0xFFu) / 255.f;
        out[2] = ((c.rgba >> 16) & 0xFFu) / 255.f;
        out[3] = ((c.rgba >> 24) & 0xFFu) / 255.f;
    }
    // Stable per-glyph [0,1) hash -> jitter phase.
    static float GlyphHash01(int glyphIndex)
    {
        uint32_t h = static_cast<uint32_t>(glyphIndex) * 374761393u + 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
    }

    // ==== Init / Shutdown ====
    bool Font::Init(IGraphicsDevice& gfx, FT_Library ft,
                    const std::string& ttfPath, float basePixelSize)
    {
        m_gfx        = &gfx;
        m_ttfPath    = ttfPath;
        m_mainThread = std::this_thread::get_id();
        m_metrics.pixelSize = basePixelSize;

        // Slurp TTF via AssetFS so packed builds find it inside game.ipak.
        // The bytes MUST outlive the face (FT memory faces don't copy).
        if (!Resource::AssetFS::Get().ReadFile(ttfPath, m_ttfBytes) || m_ttfBytes.empty())
        {
            LOG_ERROR("Font: TTF not found at %s", ttfPath.c_str());
            return false;
        }

        if (FT_New_Memory_Face(ft, m_ttfBytes.data(),
                               static_cast<FT_Long>(m_ttfBytes.size()), 0, &m_face) != 0)
        {
            LOG_ERROR("Font: FT_New_Memory_Face failed for %s", ttfPath.c_str());
            m_face = nullptr;
            return false;
        }
        if (FT_Set_Pixel_Sizes(m_face, 0, static_cast<FT_UInt>(basePixelSize)) != 0)
        {
            LOG_ERROR("Font: FT_Set_Pixel_Sizes(%.1f) failed for %s",
                      basePixelSize, ttfPath.c_str());
            FT_Done_Face(m_face);
            m_face = nullptr;
            return false;
        }

        // Global metrics (FT 26.6 fixed point -> pixels via /64).
        m_metrics.lineHeight = static_cast<float>(m_face->size->metrics.height) / 64.f;
        m_metrics.ascender   = static_cast<float>(m_face->size->metrics.ascender) / 64.f;
        m_metrics.descender  = -static_cast<float>(m_face->size->metrics.descender) / 64.f;
        m_metrics.sdfPixelRange = 2.0f * static_cast<float>(kSdfSpread);

        // Prewarm printable ASCII so the first frame (loading screen included)
        // does not hitch, then push the page to the GPU right away.
        // Init is not boot-only: FontLibrary::Resolve -> GetOrLoad -> Init runs
        // from the UI systems every frame, so a font first referenced by a script
        // or a scene opens MID-FRAME. The flush below queues the prewarmed rect
        // instead of waiting for it: mid-frame (the UI paint, inside the upload
        // window) it lands in this frame, at boot it is carried into the first;
        // FlushPendingUploads carries the game-half guard either way.
        // -font_prewarm=serial bakes one glyph at a time; either way the result is
        // logged as a hash over the atlas pixels and the glyph table, so the two
        // arms can be held to identical output (tests/unit/test_font_prewarm.py);
        // layout= and pixels= are its two halves (HashAtlas). Both arms render
        // NOW, never reserve: the hash reads the pixels right away.
        const bool serial = Sys::CommandLine::Get().Get("-font_prewarm", "parallel") == "serial";
        if (serial)
        {
            for (uint32_t cp = 0x0020u; cp <= 0x007Eu; ++cp)
                if (!m_glyphs.count(cp) && HasCodepoint(cp)) BakeGlyphNow(cp);
        }
        else
            PrewarmRange(0x0020u, 0x007Eu);
        {
            const AtlasHash h = HashAtlas();
            LOG_INFO("[FONT] prewarm %s mode=%s glyphs=%zu pages=%u hash=%016llx layout=%016llx pixels=%016llx",
                     ttfPath.c_str(), serial ? "serial" : "parallel", m_glyphs.size(), PageCount(),
                     static_cast<unsigned long long>(h.combined), static_cast<unsigned long long>(h.layout),
                     static_cast<unsigned long long>(h.pixels));
        }
        // The first check of the prediction: the prewarm RENDERED its glyphs, so
        // it says whether the cells predicted from the outlines are the ones
        // FreeType makes for this font. Only then do its later glyphs reserve.
        m_reserveFirst = PredictionsHold();
        FlushPendingUploads();

        LOG_SUCCESS("Font: opened %s @ %.1fpx (%zu glyphs prewarmed, %u page(s))",
                    ttfPath.c_str(), basePixelSize, m_glyphs.size(), PageCount());
        return true;
    }

    void Font::Shutdown()
    {
        if (m_face) { FT_Done_Face(m_face); m_face = nullptr; }
        if (m_gfx)
            for (auto& pg : m_pages)
                if (pg.tex.IsValid()) m_gfx->DestroyTexture(pg.tex);
        m_pages.clear();
        m_glyphs.clear();
        m_fallback.clear();
        m_sdfFailures.clear();
        m_pending.clear();
        m_ttfBytes.clear();
    }

    void Font::AssertMainThread() const
    {
        // EnsureGlyph mutates the glyph map, the page pixels and the reserved-cell
        // list; render passes may record on worker threads and must use
        // FindGlyph / MeasureResident. A check, not just an ERROR line: once per
        // process, CHECK FAILED.
        ENGINE_CHECK_ONCE(std::this_thread::get_id() == m_mainThread,
                          "Font: EnsureGlyph called off the main thread (%s) - "
                          "use FindGlyph/MeasureResident from render passes", m_ttfPath.c_str());
    }

    bool Font::HasCodepoint(uint32_t cp) const
    {
        return m_face && FT_Get_Char_Index(m_face, cp) != 0;
    }


// The font atlas is game-half only, and this macro makes that a check.
// Every Font touch - the bake (StoreCell), the lane build and the flush -
// runs above the game/render seam, so the atlas needs no collect/upload
// split (a split would create the very race it would solve). The upload
// sites it guards: FlushPendingUploads, Font::Init's flush (which routes
// through it), and AddPage's CreateTexture, reached mid-frame through
// StoreCell -> PackCell when a glyph opens a new page (initial data only
// under -font_syncupload, but it still creates a resource the flush then
// streams into).
// A macro, not a function: ENGINE_CHECK_ONCE's once-flag is a static at
// the point of expansion, so one shared helper would give every site one
// flag, and whichever fired first would silence the rest.
// Render is the forbidden context: the editor's ImGui block (which reaches
// Font through the font editor window) runs under Exempt, and the editor
// is pinned serial anyway.
#define FONT_ASSERT_GAME_HALF(site)                                                ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,             "[SPLITCHECK][FONT] " site " ran in the Render context. The glyph "            "atlas is game-half-only: its CPU pixel mirror is written by StoreCell "        "on the game thread with no synchronisation, so uploading it from the "         "render half reads a buffer that is being written.")

// The game half is not enough for the sites that reserve and integrate
// cells: a ParallelFor chunk inherits its caller's context, so a worker
// that reached one of them would pass FONT_ASSERT_GAME_HALF. They must
// also run on the font's main thread (the one Init ran on). A macro for
// the same reason: one once-flag per site.
#define FONT_ASSERT_MAIN(site, mainId)                                                 \
    ENGINE_CHECK_ONCE(std::this_thread::get_id() == (mainId),                          \
        "[SPLITCHECK][FONT] " site " ran off the font's main thread. The glyph "        \
        "table, the reserved cells and the page mirrors have one writer, the main "     \
        "thread; a worker renders into a buffer of its own and nothing else.")

    // ==== Atlas paging ====
    bool Font::AddPage()
    {
        // Created WITHOUT initial data. D3D12 zero-fills a committed texture
        // (CreateTexture uses CREATE_NOT_ZEROED for render targets only), which
        // matches the CPU mirror's zeros, so a new page uploads and waits for
        // nothing; the cells baked into it stream with the next flush.
        // -font_syncupload: created with its data, a mid-frame flush.
        FONT_ASSERT_GAME_HALF("Font::AddPage");
        if (m_pages.size() >= 0xFFFFu) return false; // FontGlyph::page is uint16

        AtlasPage pg;
        pg.pixels.assign(static_cast<size_t>(kPageSize) * kPageSize, 0u);
        pg.penX = pg.penY = kPad;

        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.width      = kPageSize;
        td.height     = kPageSize;
        td.depth      = 1;
        td.format     = RHI::Format::R8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;

        RHI::SubresourceData sub{};
        sub.dataPtr    = pg.pixels.data();
        sub.rowPitch   = kPageSize;
        sub.slicePitch = static_cast<uint32_t>(kPageSize) * kPageSize;

        // Created once and never destroyed before Shutdown: the SRV / bindless
        // slot must stay stable so cached handles can never alias another
        // texture (a UI image sampling the font atlas).
        if (!m_gfx->CreateTexture(td, pg.tex, FontSyncUpload() ? &sub : nullptr))
        {
            LOG_ERROR("Font: atlas page CreateTexture failed (%s)", m_ttfPath.c_str());
            return false;
        }
        pg.srv = m_gfx->GetTextureSRVGpuHandle(pg.tex);

        m_pages.push_back(std::move(pg));
        LOG_INFO("Font: %s atlas page %u added (%ux%u R8)",
                 m_ttfPath.c_str(), PageCount() - 1u, kPageSize, kPageSize);
        return true;
    }

    bool Font::PackCell(uint32_t cellW, uint32_t cellH,
                        uint32_t& outPage, uint32_t& outX, uint32_t& outY)
    {
        if (cellW + 2u * kPad > kPageSize || cellH + 2u * kPad > kPageSize)
            return false; // glyph bigger than a whole page: bake size too large
        if (m_pages.empty() && !AddPage())
            return false;

        for (;;)
        {
            AtlasPage& pg = m_pages.back(); // only the newest page keeps a live pen
            if (pg.penX + cellW + kPad > kPageSize)
            {
                pg.penX  = kPad;
                pg.penY += pg.rowH + kPad;
                pg.rowH  = 0;
            }
            if (pg.penY + cellH + kPad > kPageSize)
            {
                if (!AddPage()) return false;
                continue;
            }
            outPage = PageCount() - 1u;
            outX    = pg.penX;
            outY    = pg.penY;
            pg.penX += cellW + kPad;
            if (cellH > pg.rowH) pg.rowH = cellH;
            return true;
        }
    }

    const FontGlyph* Font::StoreCell(uint32_t cp, const std::vector<uint8_t>& sdf,
                                     uint32_t cellW, uint32_t cellH,
                                     float bearingX, float bearingY, float advance)
    {
        uint32_t page = 0, px = 0, py = 0;
        if (!PackCell(cellW, cellH, page, px, py))
        {
            LOG_WARNING("Font: %s failed to pack U+%04X (%ux%u cell)",
                        m_ttfPath.c_str(), cp, cellW, cellH);
            return nullptr;
        }
        WriteCell(page, px, py, cellW, cellH, sdf.data());
        return &m_glyphs.emplace(cp, CellGlyph(page, px, py, cellW, cellH, bearingX, bearingY, advance))
                    .first->second;
    }

    void Font::WriteCell(uint32_t page, uint32_t px, uint32_t py, uint32_t cellW, uint32_t cellH,
                         const uint8_t* src)
    {
        AtlasPage& pg = m_pages[page];
        for (uint32_t y = 0; y < cellH; ++y)
            std::memcpy(pg.pixels.data() + (static_cast<size_t>(py) + y) * kPageSize + px,
                        src + static_cast<size_t>(y) * cellW, cellW);
        // The flush streams the union of the cells written since the last one:
        // a frame's glyphs are packed side by side on the shelf, so it is a row
        // or two of the page, not the page.
        if (!pg.dirty)
        {
            pg.dirtyX0 = px;         pg.dirtyY0 = py;
            pg.dirtyX1 = px + cellW; pg.dirtyY1 = py + cellH;
        }
        else
        {
            pg.dirtyX0 = (std::min)(pg.dirtyX0, px);
            pg.dirtyY0 = (std::min)(pg.dirtyY0, py);
            pg.dirtyX1 = (std::max)(pg.dirtyX1, px + cellW);
            pg.dirtyY1 = (std::max)(pg.dirtyY1, py + cellH);
        }
        pg.dirty = true;
    }

    FontGlyph Font::CellGlyph(uint32_t page, uint32_t px, uint32_t py, uint32_t cellW, uint32_t cellH,
                              float bearingX, float bearingY, float advance)
    {
        const float invSize = 1.f / static_cast<float>(kPageSize);
        FontGlyph fg{};
        fg.uv0     = { static_cast<float>(px)         * invSize,
                       static_cast<float>(py)         * invSize };
        fg.uv1     = { static_cast<float>(px + cellW) * invSize,
                       static_cast<float>(py + cellH) * invSize };
        fg.size    = { static_cast<float>(cellW), static_cast<float>(cellH) };
        fg.bearing = { bearingX, bearingY };
        fg.advance = advance;
        fg.page    = static_cast<uint16_t>(page);
        return fg;
    }

    // ==== Per-glyph baking ====
    bool Font::s_useFtSdf = true;
    Font::BakeMode Font::s_bakeMode = Font::BakeMode::Parallel;
    Font::SdfSource Font::s_sdfSource = Font::SdfSource::Bsdf;

    const char* Font::BakeModeName(BakeMode m)
    {
        switch (m)
        {
            case BakeMode::Parallel: return "parallel";
            case BakeMode::Serial:   return "serial";
            case BakeMode::Next:     return "next";
        }
        return "?";
    }

    const char* Font::SdfSourceName(SdfSource s)
    {
        return s == SdfSource::Outline ? "outline" : "bsdf";
    }

    // Reserve-first needs the FreeType SDF (8SSEDT would give the same cells,
    // but a missing sdf module keeps rendering at the request) and this
    // font's predictions confirmed by its own prewarm. The harness arm
    // -fonttest_globalflip renders at the request too.
    bool Font::ReserveFirst() const
    {
        return m_reserveFirst && s_useFtSdf && !GlobalFlipArm();
    }

    const FontGlyph* Font::BakeGlyph(uint32_t cp)
    {
        return ReserveFirst() ? ReserveGlyph(cp) : BakeGlyphNow(cp);
    }

    // The cell and the final FontGlyph now, from the outline's control box;
    // the pixels at the frame's flush. Cells are packed in request order, as
    // rendering at the request would pack them, so the atlas comes out the
    // same byte for byte. What is not an outline (an embedded bitmap, an SVG
    // glyph) renders now.
    const FontGlyph* Font::ReserveGlyph(uint32_t cp)
    {
        PROF_SCOPE("Font.Reserve");
        FONT_ASSERT_GAME_HALF("Font::ReserveGlyph");
        FONT_ASSERT_MAIN("Font::ReserveGlyph", m_mainThread);
        const MainBakeTime bakeTime;
        if (FT_Load_Char(m_face, cp, kGlyphLoad) != 0)
            return nullptr;
        FT_GlyphSlot g = m_face->glyph;
        const float advance = static_cast<float>(g->advance.x) / 64.f;
        if (GlyphDrawsNothing(g))
        {
            NoteEmpty();
            FontGlyph fge{};
            fge.advance = advance;
            return &m_glyphs.emplace(cp, fge).first->second;
        }
        if (g->format != FT_GLYPH_FORMAT_OUTLINE)
            return BakeGlyphNow(cp);

        const GlyphGeometry::Cell c = PredictCell(g);   // not Empty: GlyphDrawsNothing said so
        uint32_t page = 0, px = 0, py = 0;
        if (!PackCell(static_cast<uint32_t>(c.w), static_cast<uint32_t>(c.h), page, px, py))
        {
            LOG_WARNING("Font: %s failed to pack U+%04X (%dx%d cell)", m_ttfPath.c_str(), cp, c.w, c.h);
            return nullptr;
        }
        PendingCell pc;
        pc.cp = cp;
        pc.page = page; pc.x = px; pc.y = py;
        pc.w = c.w; pc.h = c.h; pc.left = c.left; pc.top = c.top;
        pc.forceFail = SdfFailArm(m_ttfPath, cp);
        m_pending.push_back(pc);
        GlyphStats::reserved.fetch_add(1, std::memory_order_relaxed);
        return &m_glyphs.emplace(cp, CellGlyph(page, px, py, static_cast<uint32_t>(c.w), static_cast<uint32_t>(c.h),
                                               static_cast<float>(c.left), static_cast<float>(c.top), advance))
                    .first->second;
    }

    // The first check of the prediction: Init's prewarm has just RENDERED
    // this font's ASCII, so each of those cells says whether the control-box
    // prediction is what FreeType makes here. Every one must match, size and
    // bearing both (ReserveGlyph publishes them before the render), or this
    // font renders every later glyph at its request. Nothing is reserved yet,
    // so nothing to undo. A font with no prewarmed outline (an icon font) is
    // checked only by the unit test and IntegrateCells' per-cell check.
    bool Font::PredictionsHold()
    {
        for (const auto& [cp, fg] : m_glyphs)
        {
            if (fg.size.x <= 0.f) continue;   // advance only
            if (FT_Load_Char(m_face, cp, kGlyphLoad) != 0 || m_face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
                continue;
            const GlyphGeometry::Cell c = PredictCell(m_face->glyph);
            if (static_cast<float>(c.w) != fg.size.x || static_cast<float>(c.h) != fg.size.y ||
                static_cast<float>(c.left) != fg.bearing.x || static_cast<float>(c.top) != fg.bearing.y)
            {
                LOG_WARNING("[FONT_BAKE] %s U+%04X: the prewarm rendered a %gx%g cell at (%g, %g), the outline "
                            "predicts %dx%d at (%d, %d) - this font renders every glyph when it is asked for "
                            "(no reserve-first)", m_ttfPath.c_str(), cp, fg.size.x, fg.size.y, fg.bearing.x,
                            fg.bearing.y, c.w, c.h, c.left, c.top);
                return false;
            }
        }
        return true;
    }

    const FontGlyph* Font::BakeGlyphNow(uint32_t cp)
    {
        PROF_SCOPE("Font.Bake");
        const MainBakeTime bakeTime;
        // NO_HINTING: hinting snaps outlines to the bake-size pixel grid, but
        // glyphs render at arbitrary scales here; hinted contours show up as
        // bumps once resampled. SDF baking always wants the raw outline.
        constexpr FT_Int32 kLoad = kGlyphLoad;
        if (FT_Load_Char(m_face, cp, kLoad) != 0)
            return nullptr;
        FT_GlyphSlot g = m_face->glyph;
        const float advance = static_cast<float>(g->advance.x) / 64.f;

        // Nothing to draw (a space, a zero-area outline, an empty bitmap): no
        // quad, advance only.
        if (GlyphDrawsNothing(g))
        {
            NoteEmpty();
            FontGlyph fge{};
            fge.advance = advance;
            return &m_glyphs.emplace(cp, fge).first->second;
        }

        // Preferred: FreeType's SDF renderers (FT 2.11+), sub-pixel distances so
        // edges stay smooth at any draw scale (RenderSdf: bsdf over the coverage
        // render by default, the outline's distance under -glyphsdf=outline).
        // Their byte convention matches ours (value = 0.5 + d / (2*spread), edge =
        // 128; spread set through the "sdf" / "bsdf" module properties in
        // FontLibrary::Init), and the bitmap already includes the spread padding
        // (bitmap_left/top account for it), so the cell is copied 1:1 with no
        // bearing shift.
        if (s_useFtSdf)
        {
            const BakeClock::time_point t0 = BakeClock::now();
            FT_Error err = RenderSdf(g);
            if (err == 0 && SdfFailArm(m_ttfPath, cp)) err = FT_Err_Cannot_Render_Glyph;
            if (err == 0 && g->bitmap.buffer && g->bitmap.width > 0 && g->bitmap.rows > 0)
            {
                const uint32_t cellW = g->bitmap.width;
                const uint32_t cellH = g->bitmap.rows;
                std::vector<uint8_t> cell(static_cast<size_t>(cellW) * cellH);
                for (uint32_t y = 0; y < cellH; ++y)
                {
                    const uint8_t* row = g->bitmap.buffer
                                       + static_cast<ptrdiff_t>(y) * g->bitmap.pitch;
                    std::memcpy(cell.data() + static_cast<size_t>(y) * cellW, row, cellW);
                }
                NoteRender(t0, true);
                return StoreCell(cp, cell, cellW, cellH,
                                 static_cast<float>(g->bitmap_left),
                                 static_cast<float>(g->bitmap_top),
                                 advance);
            }
            if (GlobalFlipArm())
            {
                s_useFtSdf = false;
                LOG_WARNING("Font: FT_RENDER_MODE_SDF unavailable - falling back to "
                            "8SSEDT (edges will be slightly lumpier under scaling)");
            }
            else
            {
                // This glyph alone. The sdf module is there (FontLibrary::Init
                // clears s_useFtSdf when it is not), so a render that fails here
                // failed on this outline; switching every later glyph of every
                // font would make the atlas depend on which text came first.
                // Once per codepoint: when the 8SSEDT render fails too, nothing
                // is stored, and every frame that draws the glyph asks again.
                if (std::find(m_sdfFailures.begin(), m_sdfFailures.end(), cp) == m_sdfFailures.end())
                {
                    m_sdfFailures.push_back(cp);
                    LOG_WARNING("Font: %s U+%04X: the SDF render failed (FreeType error %d, "
                                "%ux%u bitmap) - this glyph bakes via 8SSEDT", m_ttfPath.c_str(), cp,
                                static_cast<int>(err), g->bitmap.width, g->bitmap.rows);
                }
                // The failed render may have left the slot holding a bitmap in
                // place of the outline: load the outline again.
                if (FT_Load_Char(m_face, cp, kLoad) != 0)
                    return nullptr;
            }
        }

        // Fallback: coverage render + 8SSEDT. Contours are binarised to whole
        // pixels: acceptable, but inferior to the FreeType SDF. Every glyph takes
        // it when the sdf module is missing; otherwise only a glyph whose own SDF
        // render failed.
        const BakeClock::time_point t0 = BakeClock::now();
        if (FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL) != 0) return nullptr;
        const uint32_t gw = g->bitmap.width;
        const uint32_t gh = g->bitmap.rows;
        if (gw == 0 || gh == 0 || !g->bitmap.buffer)
        {
            NoteEmpty();
            FontGlyph fge{};
            fge.advance = advance;
            return &m_glyphs.emplace(cp, fge).first->second;
        }

        // Padded SDF cell: the glyph sits kSdfSpread px in from every edge so
        // outline/glow have room; the border texels read ~0 (far outside), so
        // kPad=1 between cells is bleed-free.
        const int      spread = kSdfSpread;
        const uint32_t cellW  = gw + 2u * static_cast<uint32_t>(spread);
        const uint32_t cellH  = gh + 2u * static_cast<uint32_t>(spread);

        std::vector<uint8_t> sdf;
        ComputeGlyphSDF(g->bitmap.buffer,
                        static_cast<int>(gw), static_cast<int>(gh),
                        g->bitmap.pitch, spread,
                        static_cast<int>(cellW), static_cast<int>(cellH), sdf);
        NoteRender(t0, true);

        // The stored quad is the ENLARGED cell: bearing shifts (-spread,
        // +spread), size grows by 2*spread. Advance stays the true metric.
        return StoreCell(cp, sdf, cellW, cellH,
                         static_cast<float>(g->bitmap_left) - static_cast<float>(spread),
                         static_cast<float>(g->bitmap_top)  + static_cast<float>(spread),
                         advance);
    }

    const FontGlyph* Font::BakeTofu(uint32_t cp)
    {
        PROF_SCOPE("Font.Tofu");
        const MainBakeTime bakeTime;
        // Synthetic hollow box so missing codepoints stay visible AND advance
        // the pen. Built as a coverage bitmap and pushed through the same SDF
        // path as a real glyph, so effects/scaling behave identically.
        const int boxW = std::max(4, static_cast<int>(m_metrics.pixelSize * 0.52f));
        const int boxH = std::max(5, static_cast<int>(m_metrics.pixelSize * 0.66f));
        const int t    = std::max(1, static_cast<int>(m_metrics.pixelSize / 14.f));

        const int      spread = kSdfSpread;
        const uint32_t cellW  = static_cast<uint32_t>(boxW + 2 * spread);
        const uint32_t cellH  = static_cast<uint32_t>(boxH + 2 * spread);

        // The box depends on the pixel size alone, so every box of this font is
        // the same cell byte for byte: 8SSEDT runs for the first one and the rest
        // copy it (~0.09 ms of main thread saved per box, most of what a
        // next-frame flood of missing glyphs would cost).
        if (m_tofuSdf.empty() || m_tofuBoxW != boxW || m_tofuBoxH != boxH || m_tofuStroke != t)
        {
            const BakeClock::time_point t0 = BakeClock::now();
            std::vector<uint8_t> cover(static_cast<size_t>(boxW) * boxH, 0u);
            for (int y = 0; y < boxH; ++y)
                for (int x = 0; x < boxW; ++x)
                    if (x < t || y < t || x >= boxW - t || y >= boxH - t)
                        cover[static_cast<size_t>(y) * boxW + x] = 255u;
            ComputeGlyphSDF(cover.data(), boxW, boxH, boxW, spread,
                            static_cast<int>(cellW), static_cast<int>(cellH), m_tofuSdf);
            m_tofuBoxW = boxW; m_tofuBoxH = boxH; m_tofuStroke = t;
            NoteRender(t0, false);
        }
        else
            UI::GlyphStats::tofuReused.fetch_add(1, std::memory_order_relaxed);
        UI::GlyphStats::tofu.fetch_add(1, std::memory_order_relaxed);

        return StoreCell(cp, m_tofuSdf, cellW, cellH,
                         static_cast<float>(m_metrics.pixelSize * 0.08f) - spread,
                         static_cast<float>(boxH + spread),
                         m_metrics.pixelSize * 0.68f);
    }

    // ==== Residency ====
    Font::ResolvedGlyph Font::EnsureGlyph(uint32_t cp)
    {
        if (auto it = m_glyphs.find(cp); it != m_glyphs.end())
            return { &it->second, this };

        AssertMainThread();
        if (!m_face) return {};

        if (HasCodepoint(cp))
            if (const FontGlyph* g = BakeGlyph(cp))
                return { g, this };

        for (Font* fb : m_fallback)
        {
            if (!fb || fb == this || !fb->m_face || !fb->HasCodepoint(cp)) continue;
            ResolvedGlyph rg = fb->EnsureGlyph(cp);
            if (rg.glyph) return rg;
        }

        if (const FontGlyph* g = BakeTofu(cp))
            return { g, this };
        return {};
    }

    void Font::EnsureGlyphs(const char* utf8)
    {
        if (!utf8) return;
        const char* p = utf8;
        while (*p)
        {
            const uint32_t cp = DecodeUtf8(p);
            if (cp != '\n') EnsureGlyph(cp);
        }
    }

    void Font::EnsureRange(uint32_t first, uint32_t last)
    {
        if (!m_face) return;
        for (uint32_t cp = first; cp <= last; ++cp)
        {
            if (m_glyphs.count(cp)) continue;
            if (!HasCodepoint(cp))  continue; // prewarm skips gaps: no tofu boxes
            BakeGlyph(cp);
        }
    }

    // EnsureRange for the prewarm, with the rendering spread over the workers.
    // FreeType's outline SDF costs ~1.9 ms a glyph, so the 95-glyph ASCII
    // prewarm is ~180 ms of boot when serial. A FreeType library / face is
    // single-threaded, so each chunk opens its own over the SAME read-only TTF
    // bytes (FreeType never writes a memory face's buffer) and renders cells
    // into a private slot; then THIS thread stores them in codepoint order
    // through the same StoreCell the serial path uses, so the atlas is
    // byte-identical. A glyph a worker could not render goes through
    // BakeGlyphNow, which also owns the per-glyph 8SSEDT fallback; no FreeType
    // SDF, fewer than 16 glyphs or fewer than two workers is all-serial.
    // It renders NOW, never reserves: Init hashes the pixels right after it.
    void Font::PrewarmRange(uint32_t first, uint32_t last)
    {
        if (!m_face) return;
        PROF_SCOPE("Font.Prewarm");
        const MainBakeTime bakeTime;
        std::vector<uint32_t> cps;
        for (uint32_t cp = first; cp <= last; ++cp)
            if (!m_glyphs.count(cp) && HasCodepoint(cp)) cps.push_back(cp);
        const uint32_t workers = TaskSystem::Get().GetWorkerCount();
        if (cps.empty() || !s_useFtSdf || workers < 2 || cps.size() < 16)
        {
            for (uint32_t cp : cps) BakeGlyphNow(cp);
            return;
        }

        struct Baked
        {
            bool                 ok = false, empty = false;
            float                advance = 0.f, left = 0.f, top = 0.f;
            uint32_t             w = 0, h = 0;
            std::vector<uint8_t> cell;
        };
        std::vector<Baked> baked(cps.size());
        const uint32_t chunks = std::min<uint32_t>(workers, static_cast<uint32_t>((cps.size() + 7) / 8));
        const FT_UInt  pixels = static_cast<FT_UInt>(m_metrics.pixelSize);
        try
        {
            TaskSystem::Get().ParallelFor(0, chunks, [&](uint32_t c)
            {
                FT_Library ft = nullptr;
                if (FT_Init_FreeType(&ft) != 0) return;
                SetSdfSpread(ft);   // as FontLibrary::Init
                FT_Face face = nullptr;
                if (FT_New_Memory_Face(ft, m_ttfBytes.data(), static_cast<FT_Long>(m_ttfBytes.size()), 0,
                                       &face) == 0 && FT_Set_Pixel_Sizes(face, 0, pixels) == 0)
                {
                    for (size_t i = c; i < cps.size(); i += chunks)   // strided: the chunks stay even
                    {
                        Baked& b = baked[i];
                        if (FT_Load_Char(face, cps[i], FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) != 0) continue;
                        FT_GlyphSlot g = face->glyph;
                        b.advance = static_cast<float>(g->advance.x) / 64.f;
                        if (GlyphDrawsNothing(g))   // nothing to draw, as in BakeGlyphNow
                        {
                            b.ok = b.empty = true;
                            continue;
                        }
                        if (SdfFailArm(m_ttfPath, cps[i])) continue;   // the harness's forced failure: left to BakeGlyphNow
                        PROF_SCOPE("Font.RenderCell");
                        const BakeClock::time_point t0 = BakeClock::now();
                        if (RenderSdf(g) != 0 || !g->bitmap.buffer ||
                            g->bitmap.width == 0 || g->bitmap.rows == 0)
                            continue;
                        b.w = g->bitmap.width;
                        b.h = g->bitmap.rows;
                        b.cell.resize(static_cast<size_t>(b.w) * b.h);
                        for (uint32_t y = 0; y < b.h; ++y)
                            std::memcpy(b.cell.data() + static_cast<size_t>(y) * b.w,
                                        g->bitmap.buffer + static_cast<ptrdiff_t>(y) * g->bitmap.pitch, b.w);
                        b.left = static_cast<float>(g->bitmap_left);
                        b.top  = static_cast<float>(g->bitmap_top);
                        b.ok   = true;
                        NoteRender(t0, true);
                    }
                }
                if (face) FT_Done_Face(face);
                FT_Done_FreeType(ft);
            });
        }
        catch (...)
        {
            // No workers to run it (shutdown): whatever was not rendered goes serial below.
        }

        for (size_t i = 0; i < cps.size(); ++i)
        {
            const Baked& b = baked[i];
            if (!b.ok)
                BakeGlyphNow(cps[i]);
            else if (b.empty)
            {
                NoteEmpty();
                FontGlyph fge{};
                fge.advance = b.advance;
                m_glyphs.emplace(cps[i], fge);
            }
            else
                StoreCell(cps[i], b.cell, b.w, b.h, b.left, b.top, b.advance);
        }
    }

    Font::ResolvedGlyph Font::FindGlyph(uint32_t cp) const
    {
        if (auto it = m_glyphs.find(cp); it != m_glyphs.end())
            return { &it->second, this };
        for (const Font* fb : m_fallback)
        {
            if (!fb || fb == this) continue;
            if (auto it = fb->m_glyphs.find(cp); it != fb->m_glyphs.end())
                return { &it->second, fb };
        }
        return {};
    }

    // ==== Pages / upload ====
    uint64_t Font::PageSrvHandle(uint32_t page) const
    {
        return page < m_pages.size() ? m_pages[page].srv : 0ull;
    }

    uint32_t Font::PageBindlessIndex(uint32_t page) const
    {
        if (page >= m_pages.size()) return ~0u;
        const AtlasPage& pg = m_pages[page];
        return pg.tex.IsValid() ? pg.tex.handleId : ~0u;
    }

    bool Font::HasPendingUpload() const
    {
        if (!m_pending.empty()) return true;
        for (const auto& pg : m_pages)
            if (pg.dirty) return true;
        return false;
    }

    // pixels is the combined stream's state after the pages, so the split
    // costs only the glyph table a second time. hash= (combined) is pinned by
    // ft_ui and the font unit tests.
    Font::AtlasHash Font::HashAtlas() const
    {
        constexpr uint64_t kBasis = 1469598103934665603ull;
        auto mix = [](uint64_t& h, const void* p, size_t n)
        {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        AtlasHash r{};
        uint64_t h = kBasis;
        for (const AtlasPage& pg : m_pages) mix(h, pg.pixels.data(), pg.pixels.size());
        r.pixels = h;
        std::vector<uint32_t> cps;
        cps.reserve(m_glyphs.size());
        for (const auto& [cp, g] : m_glyphs) cps.push_back(cp);
        std::sort(cps.begin(), cps.end());
        uint64_t layout = kBasis;
        for (uint32_t cp : cps)
        {
            const FontGlyph& g = m_glyphs.at(cp);
            const float f[9] = { g.uv0.x, g.uv0.y, g.uv1.x, g.uv1.y, g.size.x, g.size.y,
                                 g.bearing.x, g.bearing.y, g.advance };
            for (uint64_t* s : { &h, &layout })
            {
                mix(*s, &cp, sizeof(cp));
                mix(*s, f, sizeof(f));
                mix(*s, &g.page, sizeof(g.page));
            }
        }
        r.combined = h;
        r.layout   = layout;
        return r;
    }

    // ==== Render the reserved cells ====
    // One reserved cell's render. The main thread fills the inputs - copies of
    // everything the rendering thread reads, so a worker touches no Font - and
    // whichever thread renders it writes the outputs and nothing else.
    struct Font::RenderJob
    {
        const uint8_t*       ttf       = nullptr;   // the font's bytes (read-only after Init)
        size_t               ttfSize   = 0;
        uint32_t             pixels    = 0;         // FT_Set_Pixel_Sizes
        uint32_t             cp        = 0;
        bool                 forceFail = false;     // -fonttest_sdffail (harness)
        bool                 ok        = false;
        int                  err       = 0;         // FreeType's error; -1: rendered, but no bitmap
        int                  w = 0, h = 0, left = 0, top = 0;
        std::vector<uint8_t> cell;                  // w x h, rows packed
    };

    void Font::RenderCell(FT_Face face, RenderJob& j)
    {
        PROF_SCOPE("Font.RenderCell");
        j.ok = false;
        if (j.forceFail) { j.err = FT_Err_Cannot_Render_Glyph; return; }
        if ((j.err = FT_Load_Char(face, j.cp, kGlyphLoad)) != 0) return;
        FT_GlyphSlot g = face->glyph;
        const BakeClock::time_point t0 = BakeClock::now();
        if ((j.err = RenderSdf(g)) != 0) return;
        j.w = static_cast<int>(g->bitmap.width);
        j.h = static_cast<int>(g->bitmap.rows);
        if (!g->bitmap.buffer || j.w == 0 || j.h == 0) { j.err = -1; return; }
        CopyBitmap(g, j.cell);
        j.left = g->bitmap_left;
        j.top  = g->bitmap_top;
        j.ok   = true;
        NoteRender(t0, true);
    }

    void Font::RenderPending(Font* const* fonts, size_t count)
    {
        size_t n = 0;
        for (size_t f = 0; f < count; ++f) n += fonts[f]->m_pending.size();
        if (n == 0) return;
        PROF_SCOPE("Font.RenderPending");
        FONT_ASSERT_GAME_HALF("Font::RenderPending");
        FONT_ASSERT_MAIN("Font::RenderPending", fonts[0]->m_mainThread);
        const MainBakeTime bakeTime;

        std::vector<RenderJob> jobs(n);
        {
            size_t i = 0;
            for (size_t f = 0; f < count; ++f)
                for (const PendingCell& pc : fonts[f]->m_pending)
                {
                    RenderJob& j = jobs[i++];
                    j.ttf       = fonts[f]->m_ttfBytes.data();
                    j.ttfSize   = fonts[f]->m_ttfBytes.size();
                    j.pixels    = static_cast<uint32_t>(fonts[f]->m_metrics.pixelSize);
                    j.cp        = pc.cp;
                    j.forceFail = pc.forceFail;
                }
        }

        // One cell gains nothing from a worker: the main thread would wait for
        // it anyway (a trickle, a typewriter; only the next-frame contract takes
        // those off the main thread).
        const uint32_t workers = TaskSystem::Get().GetWorkerCount();
        if (n == 1 || s_bakeMode == BakeMode::Serial || workers < 2)
        {
            size_t i = 0;
            for (size_t f = 0; f < count; ++f)
                for (size_t k = 0; k < fonts[f]->m_pending.size(); ++k)
                    RenderCell(fonts[f]->m_face, jobs[i++]);
        }
        else
        {
            // As PrewarmRange: chunk c renders cells c, c+chunks, ... - strided, so
            // the costly glyphs spread over the chunks - on a FreeType library and
            // faces of its own (FreeType's are single-threaded) over the same
            // read-only bytes, opening a face at the first cell of each font the
            // stride reaches (17-35 us, under 2% of one CJK cell). The main thread
            // runs chunk 0 and any chunk no worker claimed (TaskSystem::ParallelFor),
            // so a pool busy elsewhere degrades this to the serial render, never to a
            // wait.
            const uint32_t chunks = (std::min)(workers, static_cast<uint32_t>(n));
            try
            {
                TaskSystem::Get().ParallelFor(0, chunks, [&jobs, n, chunks](uint32_t c)
                {
                    FT_Library ft = nullptr;
                    if (FT_Init_FreeType(&ft) != 0) return;   // its cells stay !ok: the main thread renders them
                    SetSdfSpread(ft);   // as FontLibrary::Init
                    struct Open { const uint8_t* ttf; FT_Face face; };
                    std::vector<Open> open;
                    for (size_t i = c; i < n; i += chunks)
                    {
                        RenderJob& j = jobs[i];
                        FT_Face face = nullptr;
                        bool    seen = false;
                        for (const Open& o : open)
                            if (o.ttf == j.ttf) { face = o.face; seen = true; break; }
                        if (!seen)
                        {
                            if (FT_New_Memory_Face(ft, j.ttf, static_cast<FT_Long>(j.ttfSize), 0, &face) != 0)
                                face = nullptr;
                            else if (FT_Set_Pixel_Sizes(face, 0, j.pixels) != 0)
                            {
                                FT_Done_Face(face);
                                face = nullptr;
                            }
                            open.push_back({ j.ttf, face });   // a face that would not open: its cells go to main
                        }
                        if (face) RenderCell(face, j);
                    }
                    for (const Open& o : open)
                        if (o.face) FT_Done_Face(o.face);
                    FT_Done_FreeType(ft);
                });
            }
            catch (...)
            {
                // No workers to run it (shutdown): what was not rendered is
                // rendered below, on this thread.
            }
        }

        size_t i = 0;
        for (size_t f = 0; f < count; ++f)
        {
            Font& font = *fonts[f];
            const size_t k = font.m_pending.size();
            font.IntegrateCells(font.m_pending.data(), jobs.data() + i, k);
            font.m_pending.clear();
            i += k;
        }
    }

    // The 8SSEDT fallback makes the same cell: its coverage bitmap is the box
    // the outline SDF pads.
    void Font::IntegrateCells(const PendingCell* cells, RenderJob* jobs, size_t n)
    {
        FONT_ASSERT_MAIN("Font::IntegrateCells", m_mainThread);
        for (size_t k = 0; k < n; ++k)
        {
            const PendingCell& pc = cells[k];
            RenderJob&         j  = jobs[k];
            if (!j.ok && !j.forceFail)
                RenderCell(m_face, j);
            if (!j.ok)
            {
                // The SDF render failed on this glyph: 8SSEDT for it alone
                // (BakeGlyphNow's policy), warned once per codepoint.
                if (std::find(m_sdfFailures.begin(), m_sdfFailures.end(), pc.cp) == m_sdfFailures.end())
                {
                    m_sdfFailures.push_back(pc.cp);
                    LOG_WARNING("Font: %s U+%04X: the SDF render failed (FreeType error %d, %dx%d bitmap) - "
                                "this glyph bakes via 8SSEDT", m_ttfPath.c_str(), pc.cp, j.err, j.w, j.h);
                }
                FT_GlyphSlot g = m_face->glyph;
                const BakeClock::time_point t0 = BakeClock::now();
                if (FT_Load_Char(m_face, pc.cp, kGlyphLoad) == 0 && FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL) == 0 &&
                    g->bitmap.buffer && g->bitmap.width > 0 && g->bitmap.rows > 0)
                {
                    const int s = kSdfSpread;
                    j.w    = static_cast<int>(g->bitmap.width) + 2 * s;
                    j.h    = static_cast<int>(g->bitmap.rows) + 2 * s;
                    j.left = g->bitmap_left - s;
                    j.top  = g->bitmap_top + s;
                    ComputeGlyphSDF(g->bitmap.buffer, static_cast<int>(g->bitmap.width),
                                    static_cast<int>(g->bitmap.rows), g->bitmap.pitch, s, j.w, j.h, j.cell);
                    j.ok = true;
                    NoteRender(t0, true);
                }
            }

            auto it = m_glyphs.find(pc.cp);
            if (it == m_glyphs.end()) continue;   // reserved, so always there
            FontGlyph& fg = it->second;
            if (j.ok && j.w == pc.w && j.h == pc.h && j.left == pc.left && j.top == pc.top)
            {
                WriteCell(pc.page, pc.x, pc.y, static_cast<uint32_t>(pc.w), static_cast<uint32_t>(pc.h),
                          j.cell.data());
                continue;
            }

            // The last check of the prediction: the cell the render made is not the
            // one reserved, whose size and bearing are already published. Never
            // clipped to fit: packed again elsewhere (the reserved cell stays blank),
            // so the layout moves and every pinned atlas hash fails loudly. With
            // nothing rendered at all it stores what the request-time bake would: an
            // advance and no cell.
            GlyphStats::geomMismatches.fetch_add(1, std::memory_order_relaxed);
            if (m_mismatchLogged++ < 8)
                LOG_WARNING("[FONT_BAKE] %s U+%04X: rendered %dx%d at (%d, %d), reserved %dx%d at (%d, %d) - packed "
                            "again, so this atlas is not the request-time one", m_ttfPath.c_str(), pc.cp,
                            j.ok ? j.w : 0, j.ok ? j.h : 0, j.left, j.top, pc.w, pc.h, pc.left, pc.top);
            uint32_t page = 0, px = 0, py = 0;
            if (j.ok && PackCell(static_cast<uint32_t>(j.w), static_cast<uint32_t>(j.h), page, px, py))
            {
                WriteCell(page, px, py, static_cast<uint32_t>(j.w), static_cast<uint32_t>(j.h), j.cell.data());
                fg = CellGlyph(page, px, py, static_cast<uint32_t>(j.w), static_cast<uint32_t>(j.h),
                               static_cast<float>(j.left), static_cast<float>(j.top), fg.advance);
            }
            else
            {
                const float advance = fg.advance;
                fg = FontGlyph{};
                fg.advance = advance;
                if (!j.ok) NoteEmpty();
            }
        }
    }

    // ==== The next-frame contract (BakeMode::Next) ====
    // One flush's reserved cells, in font order and reservation order within a
    // font. Shared by the main thread and the Low-priority tasks rendering it
    // (std::shared_ptr): a task still queued when the process shuts down runs
    // during TaskSystem::Shutdown, which drains what it accepted, and touches
    // only this and the fonts' TTF bytes, which outlive the pool.
    struct Font::AsyncRender
    {
        std::vector<Font*>       owner;              // per cell
        std::vector<PendingCell> cells;
        std::vector<RenderJob>   jobs;
        std::atomic<uint32_t>    next{ 0 };          // the next cell to claim
        std::atomic<uint32_t>    remaining{ 0 };     // cells not rendered yet, claimed or not
    };

    uint32_t Font::DrainAsync(AsyncRender& g)
    {
        const uint32_t n = static_cast<uint32_t>(g.jobs.size());
        if (g.next.load(std::memory_order_acquire) >= n) return 0;   // nothing left: open nothing
        uint32_t done = 0;
        FT_Library ft = nullptr;
        bool ftTried = false;
        struct Open { const uint8_t* ttf; FT_Face face; };
        std::vector<Open> open;
        for (;;)
        {
            // One cell at a time: whoever comes next - a background task, a
            // chunk of the join, the main thread - takes the next one, so a
            // join never waits for more than the cells already being drawn.
            const uint32_t i = g.next.fetch_add(1, std::memory_order_acq_rel);
            if (i >= n) break;
            RenderJob& j = g.jobs[i];
            try
            {
                if (!ftTried)
                {
                    ftTried = true;
                    if (FT_Init_FreeType(&ft) != 0) ft = nullptr;
                    else SetSdfSpread(ft);   // as FontLibrary::Init
                }
                FT_Face face = nullptr;
                bool    seen = false;
                for (const Open& o : open)
                    if (o.ttf == j.ttf) { face = o.face; seen = true; break; }
                if (!seen && ft)
                {
                    if (FT_New_Memory_Face(ft, j.ttf, static_cast<FT_Long>(j.ttfSize), 0, &face) != 0)
                        face = nullptr;
                    else if (FT_Set_Pixel_Sizes(face, 0, j.pixels) != 0)
                    {
                        FT_Done_Face(face);
                        face = nullptr;
                    }
                    open.push_back({ j.ttf, face });
                }
                if (face) RenderCell(face, j);   // a cell left !ok is rendered again at the integration
            }
            catch (...)
            {
                j.ok = false;
            }
            ++done;
            if (g.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                g.remaining.notify_all();
        }
        for (const Open& o : open)
            if (o.face) FT_Done_Face(o.face);
        if (ft) FT_Done_FreeType(ft);
        return done;
    }

    std::shared_ptr<Font::AsyncRender> Font::StartAsync(Font* const* fonts, size_t count, uint32_t tasks)
    {
        size_t n = 0;
        for (size_t f = 0; f < count; ++f) n += fonts[f]->m_pending.size();
        if (n == 0) return nullptr;
        PROF_SCOPE("Font.StartAsync");
        FONT_ASSERT_GAME_HALF("Font::StartAsync");
        FONT_ASSERT_MAIN("Font::StartAsync", fonts[0]->m_mainThread);
        const MainBakeTime bakeTime;

        auto g = std::make_shared<AsyncRender>();
        g->owner.reserve(n);
        g->cells.reserve(n);
        g->jobs.resize(n);
        size_t i = 0;
        for (size_t f = 0; f < count; ++f)
        {
            Font& font = *fonts[f];
            for (const PendingCell& pc : font.m_pending)
            {
                RenderJob& j = g->jobs[i++];
                j.ttf       = font.m_ttfBytes.data();
                j.ttfSize   = font.m_ttfBytes.size();
                j.pixels    = static_cast<uint32_t>(font.m_metrics.pixelSize);
                j.cp        = pc.cp;
                j.forceFail = pc.forceFail;
                g->owner.push_back(&font);
                g->cells.push_back(pc);
            }
            font.m_inflight += static_cast<uint32_t>(font.m_pending.size());
            font.m_pending.clear();
        }
        g->remaining.store(static_cast<uint32_t>(n), std::memory_order_release);
        GlyphStats::asyncCells.fetch_add(n, std::memory_order_relaxed);

        // Low priority: resource loading's queue, taken only when no High work
        // waits, so the render half's graph recording goes first; and few tasks,
        // so ~2 ms cells never hold every worker. A task the pool would not take
        // leaves its cells to FinishAsync.
        const uint32_t k = (std::min)((std::max)(tasks, 1u), static_cast<uint32_t>(n));
        for (uint32_t t = 0; t < k; ++t)
        {
            try { TaskSystem::Get().Push([g] { Font::DrainAsync(*g); }, TaskSystem::TaskPriority::Low); }
            catch (...) { break; }
        }
        return g;
    }

    void Font::FinishAsync(AsyncRender& g)
    {
        const uint32_t n = static_cast<uint32_t>(g.jobs.size());
        if (n == 0) return;
        PROF_SCOPE("Font.Join");
        FONT_ASSERT_GAME_HALF("Font::FinishAsync");
        FONT_ASSERT_MAIN("Font::FinishAsync", g.owner[0]->m_mainThread);
        const MainBakeTime bakeTime;

        // 1. Cells no background task has claimed yet - a busy Low queue, a
        //    burst bigger than a frame's worth - are rendered now, on every
        //    worker and this thread as RenderPending does: the next-frame
        //    contract never costs more than the same-frame one, a frame later.
        const uint32_t claimed = (std::min)(g.next.load(std::memory_order_acquire), n);
        if (claimed < n)
        {
            std::atomic<uint32_t> joined{ 0 };
            const uint32_t left    = n - claimed;
            const uint32_t workers = TaskSystem::Get().GetWorkerCount();
            const uint32_t chunks  = (std::min)(workers, left);
            if (chunks >= 2)
            {
                try
                {
                    TaskSystem::Get().ParallelFor(0, chunks, [&g, &joined](uint32_t)
                    {
                        joined.fetch_add(DrainAsync(g), std::memory_order_relaxed);
                    });
                }
                catch (...)
                {
                    // No workers to run it (shutdown): the call below takes the rest.
                }
            }
            joined.fetch_add(DrainAsync(g), std::memory_order_relaxed);
            GlyphStats::joinCells.fetch_add(joined.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        // 2. Cells a background task is still rendering: at most one per task.
        {
            const BakeClock::time_point t0 = BakeClock::now();
            for (uint32_t r = g.remaining.load(std::memory_order_acquire); r != 0;
                 r = g.remaining.load(std::memory_order_acquire))
                g.remaining.wait(r, std::memory_order_acquire);
            const uint64_t waitNs = GlyphStats::ToNs(BakeClock::now() - t0);
            GlyphStats::joinWaitNs.fetch_add(waitNs, std::memory_order_relaxed);
            GlyphStats::NoteMax(GlyphStats::joinWaitMaxNs, waitNs);
        }

        // 3. Into the pages, font by font in reservation order - one flush
        //    after they were asked for.
        for (size_t i = 0; i < n;)
        {
            Font* font = g.owner[i];
            size_t k = i;
            while (k < n && g.owner[k] == font) ++k;
            font->IntegrateCells(g.cells.data() + i, g.jobs.data() + i, k - i);
            font->m_inflight -= static_cast<uint32_t>(k - i);
            i = k;
        }
        GlyphStats::late.fetch_add(n, std::memory_order_relaxed);
    }

    void Font::FlushPendingUploads()
    {
        FONT_ASSERT_GAME_HALF("Font::FlushPendingUploads");
        if (!m_gfx) return;
        // FontLibrary::FlushPendingUploads renders every font's reserved cells
        // before it calls this; whatever is still reserved here (a lone call)
        // is rendered before the upload, never uploaded blank.
        if (!m_pending.empty())
        {
            Font* self = this;
            RenderPending(&self, 1);
        }
        if (FontSyncUpload()) { FlushSynchronous(); return; }
        for (AtlasPage& pg : m_pages)
            if (pg.dirty) FlushStreamed(pg);
    }

    // The page's dirty rect as one MustDeliver request: copied out of the CPU
    // mirror during the call, recorded first in this frame's render half
    // (before every UI pass), never waited for. Called from App's Render
    // phase, inside the upload window, it lands in THIS frame; from a
    // boot-time Init it is carried into the first.
    bool Font::FlushStreamed(AtlasPage& pg)
    {
        const uint32_t w = pg.dirtyX1 - pg.dirtyX0, h = pg.dirtyY1 - pg.dirtyY0;
        if (w == 0 || h == 0) { pg.dirty = false; return true; }

        RHI::SubresourceData sub{};
        sub.dataPtr    = pg.pixels.data() + static_cast<size_t>(pg.dirtyY0) * kPageSize + pg.dirtyX0;
        sub.rowPitch   = kPageSize;
        sub.slicePitch = kPageSize * h;
        StreamUpload::Desc d{};
        d.planes = &sub;
        d.rect   = { pg.dirtyX0, pg.dirtyY0, w, h };
        d.policy = StreamUpload::Policy::MustDeliver;   // a glyph cell must land: never merged or dropped for room
        const StreamUpload::Result r = m_gfx->QueueTextureUpload(pg.tex, d);
        if (r == StreamUpload::Result::Rejected || r == StreamUpload::Result::Dropped)
        {
            // No uploader at all (it could not be made): this page never
            // streamed, so the synchronous path is still legal for it.
            if (r == StreamUpload::Result::Rejected && !m_gfx->GetStreamUploadStats().active)
            {
                RHI::SubresourceData whole{};
                whole.dataPtr    = pg.pixels.data();
                whole.rowPitch   = kPageSize;
                whole.slicePitch = static_cast<uint32_t>(kPageSize) * kPageSize;
                if (!m_gfx->UpdateTexture(pg.tex, &whole, 1)) return false;
            }
            else
            {
                // Kept dirty: the next frame's flush asks again.
                static std::atomic<uint32_t> s_logged{ 0 };
                if (s_logged.fetch_add(1, std::memory_order_relaxed) < 4)
                    LOG_WARNING("Font: %s page upload was %s; retried next frame", m_ttfPath.c_str(),
                                StreamUpload::ResultName(r));
                return false;
            }
        }
        pg.dirty   = false;
        pg.dirtyX0 = pg.dirtyY0 = pg.dirtyX1 = pg.dirtyY1 = 0;
        return true;
    }

    void Font::FlushSynchronous()
    {
        // Count first: this runs every frame (under -font_syncupload) and is a
        // no-op on almost all of them; an empty batch does not wait, but there is
        // still nothing to open one for.
        uint32_t dirtyPages = 0;
        for (const auto& pg : m_pages) if (pg.dirty) ++dirtyPages;
        if (dirtyPages == 0) return;

        // N dirty pages share ONE flush instead of N full drains.
        struct UploadBatch
        {
            IGraphicsDevice& g;
            explicit UploadBatch(IGraphicsDevice& gg) : g(gg) { g.BeginBufferUploadBatch(); }
            ~UploadBatch() { g.EndBufferUploadBatch(); }
            UploadBatch(const UploadBatch&) = delete;
            UploadBatch& operator=(const UploadBatch&) = delete;
        } _batch(*m_gfx);

        for (auto& pg : m_pages)
        {
            if (!pg.dirty) continue;
            // Whole-page in-place UpdateTexture: the descriptor slot never moves.
            // Synchronous (graphics-queue flush), so only frames that actually baked
            // new glyphs pay for it.
            RHI::SubresourceData sub{};
            sub.dataPtr    = pg.pixels.data();
            sub.rowPitch   = kPageSize;
            sub.slicePitch = static_cast<uint32_t>(kPageSize) * kPageSize;
            if (m_gfx->UpdateTexture(pg.tex, &sub, 1))
            {
                pg.dirty   = false;
                pg.dirtyX0 = pg.dirtyY0 = pg.dirtyX1 = pg.dirtyY1 = 0;
            }
            else
                LOG_WARNING("Font: %s page upload failed", m_ttfPath.c_str());
        }
    }

    // ==== Draw / measure ====
    Vec2 Font::DrawText(UIDrawList& out, const Vec2& pos, Color32 col,
                        const char* text, const TextDrawParams& p)
    {
        if (!text || !*text || !m_face) return pos;
        const float sizePx = p.sizePx;
        if (sizePx <= 0.f) return pos;

        // Bake anything missing NOW (CPU tick phase) so the emit passes below
        // are pure lookups. FlushPendingUploads later uploads the dirty pages.
        EnsureGlyphs(text);

        const float scale    = sizePx / m_metrics.pixelSize; // own-font units -> px
        const float refScale = sizePx / kTextRefPx;          // px-effect scale (effects are authored at kTextRefPx)
        const TextEffect& fx = p.effects;

        // Resolve outline/glow (and custom-shader params) into a per-frame
        // effect-table entry. Widths are authored in kTextRefPx-relative pixels:
        // normalise by sdfPixelRange AND compensate for the base bake size, so a
        // 2px outline looks 2px at fontScale 1 whatever pixelSize is.
        uint32_t effIdx = 0;
        if (fx.outline || fx.glow || fx.customShaderSlot != 0)
        {
            const float range = m_metrics.sdfPixelRange > 0.f ? m_metrics.sdfPixelRange : 8.f;
            const float comp  = m_metrics.pixelSize / kTextRefPx;
            GpuTextEffect ge{};
            if (fx.outline)
            {
                Color32ToFloat4(fx.outlineColor, ge.outlineColor);
                ge.outlineWidthN = std::min(0.49f, std::max(0.f, fx.outlineWidthPx * comp / range));
            }
            else { ge.outlineColor[3] = 0.f; }
            if (fx.glow)
            {
                Color32ToFloat4(fx.glowColor, ge.glowColor);
                ge.glowWidthN = std::min(0.5f, std::max(0.f, fx.glowWidthPx * comp / range));
            }
            else { ge.glowColor[3] = 0.f; }
            // Free-form params for custom text shaders (TextFX.hlsli
            // customA/customB). Harmless zeros on the built-in path.
            for (int k = 0; k < 4; ++k)
            {
                ge.customA[k] = fx.customParams[k];
                ge.customB[k] = fx.customParams[4 + k];
            }
            // Whole-run rect (canvas px) for custom shaders: lets an effect span
            // the ENTIRE string (gradients) via TextRunT() instead of per-glyph
            // atlas UVs. Only filled for custom runs so identical built-in styles
            // keep deduping into one table slot.
            if (fx.customShaderSlot != 0)
            {
                const Vec2 runSize = MeasureResident(text, sizePx);
                ge.runRect[0] = pos.x;
                ge.runRect[1] = pos.y;
                ge.runRect[2] = 1.f / std::max(runSize.x, 1e-3f);
                ge.runRect[3] = 1.f / std::max(runSize.y, 1e-3f);
            }
            effIdx = out.AddTextEffect(ge);
        }

        // One styled pass: walk the string emitting SDF glyph quads at a pixel
        // offset (ox,oy), tinted `c`, with effect index `eff`. Jitter (if on)
        // wobbles each glyph quad about its pen position; the pen itself never
        // jitters so spacing stays stable and shadow tracks the main glyph.
        Vec2 endPen{ pos.x, pos.y };
        auto emitPass = [&](float ox, float oy, Color32 c, uint32_t eff, uint32_t customSlot)
        {
            float penX       = pos.x + ox;
            float baselineY  = pos.y + m_metrics.ascender * scale + oy;
            int   glyphIndex = 0;
            const char* pch = text;
            while (*pch)
            {
                const uint32_t cp = DecodeUtf8(pch);
                if (cp == '\n')
                {
                    penX       = pos.x + ox;
                    baselineY += m_metrics.lineHeight * scale;
                    continue;
                }
                const ResolvedGlyph rg = FindGlyph(cp);
                if (rg.glyph)
                {
                    const FontGlyph& g = *rg.glyph;
                    // Fallback glyphs are in the OWNER's baked units.
                    const float gs = sizePx / rg.owner->m_metrics.pixelSize;
                    if (g.size.x > 0.f && g.size.y > 0.f)
                    {
                        float jx = 0.f, jy = 0.f;
                        if (fx.jitter)
                        {
                            const float ph  = GlyphHash01(glyphIndex) * 6.2831853f;
                            const float amp = fx.jitterAmpPx * refScale;
                            jx = amp * std::sin(p.timeSec * fx.jitterFreq + ph);
                            jy = amp * std::cos(p.timeSec * fx.jitterFreq * 1.13f + ph * 1.7f);
                        }
                        const Vec2 mn{ penX + g.bearing.x * gs + jx,
                                       baselineY - g.bearing.y * gs + jy };
                        const Vec2 mx{ mn.x + g.size.x * gs,
                                       mn.y + g.size.y * gs };
                        UITextureRef texRef{};
                        texRef.srvGpuHandle = rg.owner->PageSrvHandle(g.page);
                        out.AddImageMat(texRef, mn, mx, g.uv0, g.uv1, c, 1u, eff,
                                        0u, customSlot);
                    }
                    penX += g.advance * gs;
                }
                ++glyphIndex;
            }
            endPen = Vec2{ penX, baselineY };
        };

        // Drop shadow first (behind), then the main text on top. The shadow
        // copy stays on the built-in SDF shader (slot 0) so a custom main
        // shader still gets a plain dark silhouette behind it.
        if (fx.shadow)
            emitPass(fx.shadowOffsetPx.x * refScale, fx.shadowOffsetPx.y * refScale,
                     fx.shadowColor, 0u, 0u);
        emitPass(0.f, 0.f, col, effIdx, fx.customShaderSlot);

        return endPen;
    }

    Vec2 Font::Measure(const char* text, float sizePx)
    {
        EnsureGlyphs(text);
        return MeasureResident(text, sizePx);
    }

    Vec2 Font::MeasureResident(const char* text, float sizePx) const
    {
        if (!text || !*text || sizePx <= 0.f) return { 0, 0 };
        float lineW = 0.f;
        float maxW  = 0.f;
        int   lines = 1;
        const char* p = text;
        while (*p)
        {
            const uint32_t cp = DecodeUtf8(p);
            if (cp == '\n')
            {
                if (lineW > maxW) maxW = lineW;
                lineW = 0.f;
                ++lines;
                continue;
            }
            const ResolvedGlyph rg = FindGlyph(cp);
            if (!rg.glyph) continue;
            lineW += rg.glyph->advance * (sizePx / rg.owner->m_metrics.pixelSize);
        }
        if (lineW > maxW) maxW = lineW;
        return Vec2{ maxW,
                     static_cast<float>(lines) * m_metrics.lineHeight
                         * (sizePx / m_metrics.pixelSize) };
    }

    // ==== Word wrap ====
    namespace
    {
        // Scripts that break between any two characters because they have no
        // spaces: CJK ideographs + compatibility, Hiragana, Katakana, Hangul,
        // and the CJK symbols/punctuation block.
        bool IsCjkBreakable(uint32_t cp)
        {
            return (cp >= 0x3040 && cp <= 0x30FF)     // Hiragana + Katakana
                || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Ext A
                || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified
                || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility
                || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul syllables
                || (cp >= 0x3000 && cp <= 0x303F)     // CJK symbols/punctuation
                || (cp >= 0xFF00 && cp <= 0xFF60);    // Fullwidth forms
        }

        // A line must not START with these. Chinese/Japanese typesetting calls this
        // kinsoku shori; the subset here is the one a reader notices at a glance.
        bool IsNoBreakBefore(uint32_t cp)
        {
            switch (cp)
            {
                case 0x3001: case 0x3002:              // ideographic comma, ideographic full stop
                case 0xFF0C: case 0xFF0E: case 0xFF1A: // fullwidth comma, full stop, colon
                case 0xFF1B: case 0xFF01: case 0xFF1F: // fullwidth semicolon, exclamation mark, question mark
                case 0x3009: case 0x300B: case 0x300D: // right angle, right double angle, right corner bracket
                case 0x300F: case 0x3011: case 0x3015: // right white corner, black lenticular, tortoise shell bracket
                case 0xFF09: case 0xFF3D: case 0xFF5D: // fullwidth right parenthesis, square bracket, curly bracket
                case ')':    case ']':    case '}':
                case ',':     case '.':    case ':':
                case ';':     case '!':    case '?':
                    return true;
                default: return false;
            }
        }
    }

    std::string Font::WrapText(const char* text, float sizePx, float maxWidthPx)
    {
        if (!text || !*text) return std::string();
        std::string out(text);
        if (sizePx <= 0.f || maxWidthPx <= 0.f) return out;

        // Bake first, then measure with the bake-free lookup: the same split
        // DrawText uses, and it keeps the per-glyph loop below allocation-free.
        EnsureGlyphs(text);

        float lineW      = 0.f;
        // Byte offset of the last place a break could be inserted on this line.
        // widthAtBreak (the width up to it) is tracked but unused: after a break
        // the walk restarts from the break with lineW = 0.
        std::size_t lastBreak = std::string::npos;
        float widthAtBreak    = 0.f;

        std::size_t i = 0;
        while (i < out.size())
        {
            const char*   cursor = out.c_str() + i;
            const char*   probe  = cursor;
            const uint32_t cp    = DecodeUtf8(probe);
            const std::size_t len = static_cast<std::size_t>(probe - cursor);
            if (len == 0) break;                       // malformed: stop rather than spin

            if (cp == '\n')
            {
                lineW     = 0.f;
                lastBreak = std::string::npos;
                i += len;
                continue;
            }

            const ResolvedGlyph rg = FindGlyph(cp);
            const float adv = rg.glyph
                            ? rg.glyph->advance * (sizePx / rg.owner->m_metrics.pixelSize)
                            : 0.f;

            // Record a break opportunity BEFORE this glyph when it is CJK and does
            // not have to stay attached to what precedes it.
            if (i > 0 && IsCjkBreakable(cp) && !IsNoBreakBefore(cp))
            {
                lastBreak    = i;
                widthAtBreak = lineW;
            }

            if (lineW + adv > maxWidthPx && lineW > 0.f)
            {
                if (lastBreak != std::string::npos && lastBreak > 0)
                {
                    // Break at the recorded opportunity. A space AT the break is
                    // replaced by the newline (a trailing space before a wrap would
                    // otherwise push the visible text off-centre); anything else
                    // gets a newline inserted before it.
                    if (out[lastBreak] == ' ')
                    {
                        out[lastBreak] = '\n';
                        i     = lastBreak + 1;
                        lineW = 0.f;
                    }
                    else
                    {
                        out.insert(lastBreak, 1, '\n');
                        i     = lastBreak + 1;
                        lineW = 0.f;
                    }
                    // Re-walk from just after the break so the new line's width is
                    // measured from real glyphs rather than estimated.
                    lastBreak = std::string::npos;
                    (void)widthAtBreak;
                    continue;
                }
                // No opportunity on this line at all (one long unbreakable run):
                // hard-break here rather than overflow the rect.
                out.insert(i, 1, '\n');
                i    += 1 + len;
                lineW = adv;
                lastBreak = std::string::npos;
                continue;
            }

            lineW += adv;
            i     += len;

            // A space is a break opportunity AFTER it, so the space stays on the
            // line it ends.
            if (cp == ' ')
            {
                lastBreak    = i - len;
                widthAtBreak = lineW;
            }
            else if (IsCjkBreakable(cp))
            {
                // Also breakable after a CJK glyph, provided what follows may start
                // a line.
                const char* nxt = out.c_str() + i;
                if (*nxt)
                {
                    const char* np = nxt;
                    const uint32_t ncp = DecodeUtf8(np);
                    if (!IsNoBreakBefore(ncp)) { lastBreak = i; widthAtBreak = lineW; }
                }
            }
        }
        return out;
    }

} // namespace UI
