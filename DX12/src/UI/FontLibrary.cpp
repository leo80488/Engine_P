#include "UI/FontLibrary.h"
#include "UI/GlyphStats.h"   // LogBakeSummary
#include "System/CommandLine.h"   // -glyphbake
#include "System/Log.h"
#include "System/TaskSystem.h"    // the [FONT_BAKE] mode line's worker count; -fonttest_lowload
#include "System/PathUtil.h"      // Sys::HasArtifactDir: a harness run draws new glyphs same-frame

#include <chrono>
#include <cstdlib>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H   // FT_Property_Set (sdf/bsdf spread)

#include <algorithm>

namespace UI
{
    FontLibrary& FontLibrary::Get()
    {
        static FontLibrary s_lib;
        return s_lib;
    }

    // Shim declared in Font.h: the default font without naming FontLibrary.
    Font& DefaultFont()
    {
        return FontLibrary::Get().Default();
    }

    std::string FontLibrary::NormalizeKey(const std::string& path)
    {
        std::string key = path;
        std::replace(key.begin(), key.end(), '\\', '/');
        return key;
    }

    bool FontLibrary::Init(IGraphicsDevice& gfx, const std::string& defaultTtfPath,
                           float basePixelSize)
    {
        m_gfx           = &gfx;
        m_basePixelSize = basePixelSize > 0.f ? basePixelSize : kDefaultBasePixelSize;

        if (!m_ft && FT_Init_FreeType(&m_ft) != 0)
        {
            LOG_ERROR("FontLibrary: FT_Init_FreeType failed");
            m_ft = nullptr;
            return false;
        }

        // Match FreeType's SDF renderer to the atlas convention: alpha 0.5 = edge,
        // across +-kSdfSpread px (sdfPixelRange = 2*spread). If the sdf module is
        // missing the property set fails and every glyph of every font falls back
        // to the coverage + 8SSEDT path - the one case that switches them all (a
        // glyph whose own SDF render fails falls back alone: Font::BakeGlyphNow,
        // Font::IntegrateCells).
        FT_Int spread = Font::kSdfSpread;
        if (FT_Property_Set(m_ft, "sdf", "spread", &spread) != 0)
        {
            Font::s_useFtSdf = false;
            LOG_WARNING("FontLibrary: FreeType sdf module unavailable — "
                        "glyphs bake via 8SSEDT fallback");
        }
        // bsdf: the SDF renderer over a bitmap - the coverage render by
        // default, an embedded strike always.
        const bool bsdfModule = FT_Property_Set(m_ft, "bsdf", "spread", &spread) == 0;

        // How an on-demand glyph's pixels are rendered (Font::BakeMode). Read
        // here, before the first font opens: the flag belongs to the feature, not
        // to App (source_gates). Without it a harness run (-artifacts or
        // -unattended: packs, goldens, agent sessions, every picture something
        // compares) draws a new glyph in the frame that asks for it (parallel);
        // any other run (the game, the editor in a person's hands) draws it one
        // frame later (next) and keeps its render off the main thread, a lone new
        // glyph a frame included.
        {
            const auto& cl = Sys::CommandLine::Get();
            const unsigned workers = TaskSystem::Get().GetWorkerCount();
            const bool harness = Sys::HasArtifactDir() || cl.Has("-unattended");
            const std::string mode = cl.Get("-glyphbake", "");
            const bool named = mode == "serial" || mode == "next" || mode == "parallel";
            if (mode == "serial")    Font::s_bakeMode = Font::BakeMode::Serial;
            else if (mode == "next") Font::s_bakeMode = Font::BakeMode::Next;
            else if (named)          Font::s_bakeMode = Font::BakeMode::Parallel;
            else
            {
                if (!mode.empty())
                    LOG_WARNING("[FONT_BAKE] -glyphbake=%s is not parallel, serial or next - the default",
                                mode.c_str());
                Font::s_bakeMode = harness ? Font::BakeMode::Parallel : Font::BakeMode::Next;
            }
            // How many Low-priority tasks render one flush's cells under next
            // (default: a quarter of the workers).
            m_asyncTasks = static_cast<uint32_t>((std::max)(1, cl.GetInt("-glyphbake_jobs",
                                                     static_cast<int>((std::max)(1u, workers / 4)))));

            // Which SDF renderer draws a cell: bsdf (the coverage render, then the
            // SDF of that bitmap, 2.5x cheaper) unless -glyphsdf=outline names the
            // SDF measured from the outline. A FreeType without the bsdf module
            // draws the outline SDF.
            const std::string sdf = cl.Get("-glyphsdf", "");
            if (!sdf.empty() && sdf != "bsdf" && sdf != "outline")
                LOG_WARNING("[FONT_BAKE] -glyphsdf=%s is not bsdf or outline - the default", sdf.c_str());
            Font::s_sdfSource = (sdf == "outline" || !bsdfModule) ? Font::SdfSource::Outline
                                                                  : Font::SdfSource::Bsdf;
            const char* sdfWhy = !bsdfModule && sdf != "outline"     ? "no bsdf module"
                               : sdf == "outline" || sdf == "bsdf"     ? "-glyphsdf"
                                                                       : "default";
            LOG_INFO("[FONT_BAKE] mode=%s (%s) workers=%u jobs=%u sdf=%s (%s)",
                     Font::BakeModeName(Font::s_bakeMode),
                     named ? "-glyphbake" : (harness ? "default: a harness run" : "default: an interactive run"),
                     workers, m_asyncTasks, Font::SdfSourceName(Font::s_sdfSource), sdfWhy);
        }

        Font* def = GetOrLoad(defaultTtfPath, m_basePixelSize);
        if (!def)
        {
            LOG_ERROR("FontLibrary: default font failed to load (%s)",
                      defaultTtfPath.c_str());
            return false;
        }
        LOG_SUCCESS("FontLibrary: default font %s ready", defaultTtfPath.c_str());
        return true;
    }

    void FontLibrary::Shutdown()
    {
        for (auto& f : m_fonts)
            if (f) f->Shutdown();
        m_fonts.clear();
        m_failed.clear();
        if (m_ft) { FT_Done_FreeType(m_ft); m_ft = nullptr; }
        m_gfx = nullptr;
    }

    Font* FontLibrary::GetOrLoad(const std::string& ttfPath, float basePixelSize)
    {
        if (!m_gfx || !m_ft || ttfPath.empty()) return nullptr;

        const std::string key = NormalizeKey(ttfPath);
        for (auto& f : m_fonts)
            if (f && f->TTFPath() == key) return f.get();
        if (m_failed.count(key)) return nullptr;

        auto font = std::make_unique<Font>();
        const float px = basePixelSize > 0.f ? basePixelSize : m_basePixelSize;
        if (!font->Init(*m_gfx, m_ft, key, px))
        {
            // Negative-cache so a bad component path logs once, not per frame.
            m_failed.insert(key);
            return nullptr;
        }

        // Every non-default font falls back to the default for codepoints its
        // face lacks (icons fonts stay mixable, CJK stays visible).
        if (!m_fonts.empty())
            font->SetFallbacks({ m_fonts[0].get() });

        m_fonts.push_back(std::move(font));
        return m_fonts.back().get();
    }

    Font& FontLibrary::Resolve(const std::string& ttfPath)
    {
        if (!ttfPath.empty())
            if (Font* f = GetOrLoad(ttfPath))
                return *f;
        return Default();
    }

    Font* FontLibrary::Find(const std::string& ttfPath) const
    {
        if (ttfPath.empty()) return nullptr;
        const std::string key = NormalizeKey(ttfPath);
        for (const auto& f : m_fonts)
            if (f && f->TTFPath() == key) return f.get();
        return nullptr;
    }

    Font& FontLibrary::Default()
    {
        // Entry 0 is the default font loaded by Init. A static dummy keeps
        // pre-Init callers safe (IsReady() == false -> callers skip drawing).
        if (!m_fonts.empty()) return *m_fonts[0];
        static Font s_null;
        return s_null;
    }

    void FontLibrary::FlushPendingUploads()
    {
        // Next-frame mode: the cells the previous flush handed off are finished
        // and written first - they upload below, one frame after they were asked
        // for - and this frame's are handed off without waiting.
        if (Font::s_bakeMode == Font::BakeMode::Next) FinishAsyncRender();

        // The frame's reserved cells of EVERY font in one render (one
        // ParallelFor across fonts) or one hand-off (next), then each font
        // streams what it wrote. The vector allocates only on a frame that
        // reserved something.
        std::vector<Font*> reserved;
        for (auto& f : m_fonts)
            if (f && f->ReservedCells() != 0) reserved.push_back(f.get());
        if (!reserved.empty())
        {
            if (Font::s_bakeMode == Font::BakeMode::Next)
            {
                LowLoadArm();
                m_async = Font::StartAsync(reserved.data(), reserved.size(), m_asyncTasks);
            }
            else
                Font::RenderPending(reserved.data(), reserved.size());
        }
        for (auto& f : m_fonts)
            if (f) f->FlushPendingUploads();
    }

    void FontLibrary::FinishAsyncRender()
    {
        if (!m_async) return;
        const std::shared_ptr<Font::AsyncRender> g = std::move(m_async);
        m_async.reset();
        Font::FinishAsync(*g);
    }

    // -fonttest_lowload=<tasks>:<ms> (harness): ahead of every next-frame
    // hand-off, <tasks> Low-priority tasks of <ms> busy time each - a loading
    // queue the glyph tasks wait behind, so the next flush finds their cells
    // unclaimed. The join must then render them itself and wait for nothing
    // it did not start.
    void FontLibrary::LowLoadArm()
    {
#ifdef WITH_HARNESS
        struct Spec { int tasks = 0; double ms = 0.0; };
        static const Spec s_spec = []
        {
            Spec s;
            const std::string arg = Sys::CommandLine::Get().Get("-fonttest_lowload");
            const size_t colon = arg.find(':');
            if (colon != std::string::npos)
            {
                s.tasks = std::atoi(arg.substr(0, colon).c_str());
                s.ms    = std::atof(arg.c_str() + colon + 1);
            }
            if (!arg.empty() && (s.tasks <= 0 || s.ms <= 0.0))
            {
                LOG_WARNING("[FONT_BAKE] -fonttest_lowload=%s is not <tasks>:<ms> - no load", arg.c_str());
                s = Spec{};
            }
            return s;
        }();
        for (int i = 0; i < s_spec.tasks; ++i)
        {
            const double ms = s_spec.ms;
            TaskSystem::Get().Push([ms]
            {
                const auto until = std::chrono::steady_clock::now() +
                                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double, std::milli>(ms));
                while (std::chrono::steady_clock::now() < until) YieldProcessor();
            }, TaskSystem::TaskPriority::Low);
        }
#endif
    }

    void FontLibrary::SetFallbackChain(Font& font, std::vector<Font*> chain)
    {
        font.SetFallbacks(std::move(chain));
    }

    void FontLibrary::LogBakeSummary() const
    {
        namespace GS = GlyphStats;
        size_t glyphs = 0;
        uint32_t pages = 0;
        for (const auto& f : m_fonts)
            if (f) { glyphs += f->Glyphs().size(); pages += f->PageCount(); }
        LOG_INFO("[FONT_BAKE] summary fonts=%zu glyphs=%zu pages=%u reserved=%llu rendered=%llu "
                 "renderedOffMain=%llu empty=%llu tofu=%llu tofuReused=%llu late=%llu geomMismatches=%llu mainMs=%.3f "
                 "workerMs=%.3f renderMs=%.3f asyncCells=%llu joinCells=%llu joinWaitMs=%.3f "
                 "joinWaitMaxMs=%.3f renderMaxMs=%.3f",
                 m_fonts.size(), glyphs, pages,
                 static_cast<unsigned long long>(GS::Load(GS::reserved)),
                 static_cast<unsigned long long>(GS::Load(GS::rendered)),
                 static_cast<unsigned long long>(GS::Load(GS::renderedOffMain)),
                 static_cast<unsigned long long>(GS::Load(GS::empty)),
                 static_cast<unsigned long long>(GS::Load(GS::tofu)),
                 static_cast<unsigned long long>(GS::Load(GS::tofuReused)),
                 static_cast<unsigned long long>(GS::Load(GS::late)),
                 static_cast<unsigned long long>(GS::Load(GS::geomMismatches)),
                 GS::NsToMs(GS::Load(GS::mainNs)), GS::NsToMs(GS::Load(GS::workerNs)),
                 GS::NsToMs(GS::Load(GS::renderNs)),
                 static_cast<unsigned long long>(GS::Load(GS::asyncCells)),
                 static_cast<unsigned long long>(GS::Load(GS::joinCells)),
                 GS::NsToMs(GS::Load(GS::joinWaitNs)), GS::NsToMs(GS::Load(GS::joinWaitMaxNs)),
                 GS::NsToMs(GS::Load(GS::renderMaxNs)));
    }

} // namespace UI
