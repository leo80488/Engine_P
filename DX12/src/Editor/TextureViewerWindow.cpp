// TextureViewerWindow.cpp - see the header.
#include "Editor/TextureViewerWindow.h"

#include "Editor/EditorTheme.h"
#include "Editor/FontAwesomeIcons.h"
#include "ECS/ECS.h"
#include "Graphics/FormatNames.h"
#include "Graphics/IGraphicsDevice.h"
#include "Harness/ControlChannelJobs.h"
#include "Renderer/Renderer.h"
#include "Renderer/TextureViewService.h"
#include "Resource/AssetRefs.h"
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/TaskSystem.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace Editor
{
namespace
{
    constexpr std::uint64_t kSettleFrames = 4;     // no redraw for this long = final
    constexpr std::uint64_t kShotDeadline = 400;   // editor frames a shot may wait
    constexpr float         kInfoWidth    = 300.f;
    constexpr float         kMinZoom      = 1.f / 256.f;
    constexpr float         kMaxZoom      = 256.f;
    constexpr std::uint32_t kDefaultOut   = 512;   // the output with no canvas (a harness run)

    enum StepKind { kStepView = 1, kStepShot, kStepClose, kStepProbe, kStepStats };

    std::string Vec4Text(const float* v)
    {
        char b[128];
        std::snprintf(b, sizeof(b), "%.6g,%.6g,%.6g,%.6g", v[0], v[1], v[2], v[3]);
        return b;
    }
    std::string Vec4Text(const std::array<double, 4>& v)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "%.6g,%.6g,%.6g,%.6g", v[0], v[1], v[2], v[3]);
        return b;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string BaseName(const std::string& p)
    {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    // A harness file name: relative = under the run's diagnostics folder.
    std::string HarnessPath(const std::string& p)
    {
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
        return absolute ? p : Sys::DiagnosticsDir() + "/" + p;
    }

    // "<value>@<frame>" -> value, frame. No '@' = frame 0.
    bool SplitAt(const std::string& item, std::string& value, std::uint64_t& at)
    {
        const std::size_t p = item.find_last_of('@');
        if (p == std::string::npos) { value = item; at = 0; return !value.empty(); }
        value = item.substr(0, p);
        const std::string f = item.substr(p + 1);
        if (value.empty() || f.empty() ||
            !std::all_of(f.begin(), f.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            return false;
        at = std::strtoull(f.c_str(), nullptr, 10);
        return true;
    }

    std::vector<std::string> SplitList(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t b = 0;
        while (b <= s.size())
        {
            const std::size_t e = s.find(sep, b);
            const std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            if (!item.empty()) out.push_back(item);
            if (e == std::string::npos) break;
            b = e + 1;
        }
        return out;
    }

    bool ParseFloat(const std::string& s, float& out)
    {
        if (s.empty()) return false;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (!end || *end || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    bool ParseUInt(const std::string& s, std::uint32_t& out)
    {
        if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            return false;
        out = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
        return true;
    }

    bool ParseBool(const std::string& s, bool& out)
    {
        const std::string l = Lower(s);
        if (l == "1" || l == "true" || l == "on" || l == "yes")  { out = true;  return true; }
        if (l == "0" || l == "false" || l == "off" || l == "no") { out = false; return true; }
        return false;
    }

    bool IsFloatFormat(std::uint32_t dxgi)
    {
        switch (dxgi)
        {
        case 2: case 6: case 10: case 16: case 26: case 34: case 41: case 54: case 67: case 95: case 96:
            return true;
        default:
            return false;
        }
    }

    // A same-named source image beside the .itex, the importer's inputs.
    std::string FindSourceFile(const std::string& itexPath)
    {
        std::error_code ec;
        const std::filesystem::path p = Sys::U8Path(itexPath);
        for (const char* ext : { ".png", ".jpg", ".jpeg", ".tga", ".hdr", ".dds", ".bmp" })
        {
            std::filesystem::path q = p;
            q.replace_extension(ext);
            if (std::filesystem::exists(q, ec)) return Sys::PathToUtf8(q.filename());
        }
        return {};
    }

    ImU32 ChannelColor(int c, bool on)
    {
        static const ImU32 kOn[4]  = { IM_COL32(200, 60, 60, 255), IM_COL32(60, 170, 60, 255),
                                       IM_COL32(70, 110, 220, 255), IM_COL32(170, 170, 170, 255) };
        return on ? kOn[c] : IM_COL32(60, 60, 60, 255);
    }
}

TextureViewerWindow& TextureViewerWindow::Get()
{
    static TextureViewerWindow s_instance;
    return s_instance;
}

// ==== Tabs ====
TextureViewerWindow::Tab* TextureViewerWindow::ActiveTabPtr()
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? &m_tabs[m_active] : nullptr;
}

const TextureViewerWindow::Tab* TextureViewerWindow::ActiveTabPtr() const
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? &m_tabs[m_active] : nullptr;
}

const TextureViewerWindow::Tab* TextureViewerWindow::CompareTab(const Tab& a, std::uint32_t& bMip) const
{
    if (a.view.compare == TextureView::Compare::Off) return nullptr;
    const std::uint32_t mip = m_harnessMipOff ? 0u : a.view.mip;
    if (a.view.compareWith.empty())
    {
        // Mip N against N+1 (the last mip against itself).
        bMip = (std::min)(mip + 1u, (std::max)(1u, a.info.mipLevels) - 1u);
        return &a;
    }
    for (const Tab& b : m_tabs)
        if (b.path == a.view.compareWith)   // Set stored the tab's own path
        {
            bMip = (std::min)(mip, (std::max)(1u, b.info.mipLevels) - 1u);
            return &b;
        }
    return nullptr;
}

void TextureViewerWindow::RequestOpen(const std::string& path)
{
    m_open = true;
    m_pendingOpen.push_back(path);
}

void TextureViewerWindow::RequestImportSettings(const std::string& path)
{
    RequestOpen(path);
    m_focusImport = Sys::RelativizeAssetPath(path);
    for (char& c : m_focusImport) if (c == '\\') c = '/';
}

void TextureViewerWindow::LoadImport(Tab& t)
{
    t.importRecord = Resource::TextureImportRecordFor(t.path);
    t.importEdit   = t.importRecord.settings;
    t.importSource = t.importRecord.source;
    // A guess that could not name the format: what the source's name suggests.
    if (!t.importRecord.known && !t.importSource.empty())
        t.importEdit.format = Resource::TextureImporter::SuggestFormat(t.importSource);
    std::error_code ec;
    t.importSourceThere = !t.importSource.empty() && std::filesystem::is_regular_file(Sys::U8Path(t.importSource), ec);
    t.importLoaded = true;
}

bool TextureViewerWindow::OpenNow(const TextureViewerContext& ctx, const std::string& pathIn, std::string& why)
{
    std::string path = Sys::RelativizeAssetPath(pathIn);
    for (char& c : path) if (c == '\\') c = '/';
    if (path.empty()) { why = "a path"; return false; }
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (Lower(m_tabs[i].path) == Lower(path))
        {
            m_open     = true;
            m_active   = i;
            m_focusTab = i;
            return true;
        }
    if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
    {
        why = "already " + std::to_string(kMaxTabs) + " tabs open - close one first";
        return false;
    }
    Resource::TextureFileInfo info = Resource::DescribeTextureFile(path);
    if (!info.ok)
    {
        why = "'" + path + "' is not a readable .itex (" + info.error + ")";
        return false;
    }
    if (!ctx.textures || !ctx.resources || !ctx.gfx)
    {
        why = "no texture system";
        return false;
    }
    if (m_harnessTrustHeader)
    {
        // The reverse arm: what a viewer that believed TextureMetadata would say (the
        // dimension is written three ways on disk).
        info.isCube    = info.headerDimension == 6;
        info.dimension = info.isCube ? "Cube" : info.headerDimension == 4 ? "3D" : "2D";
        info.headerMismatch.clear();
    }
    Tab t;
    t.path   = path;
    t.name   = BaseName(path);
    t.info   = std::move(info);
    t.handle = ctx.textures->Acquire(path, *ctx.resources, *ctx.gfx);
    // The mapping the format wants first: floats HDR, BC5 a normal map, the rest as
    // stored. A cube opens as the cross (every face at once).
    if (IsFloatFormat(t.info.dxgiFormat))  t.view.map = TextureView::Map::Hdr;
    else if (t.info.dxgiFormat == 83)      t.view.map = TextureView::Map::Normal;   // BC5_UNORM
    if (t.info.isCube) t.view.cubeLayout = TextureView::CubeLayout::Cross;
    t.sourceFile = FindSourceFile(path);
    m_tabs.push_back(std::move(t));
    m_active   = static_cast<int>(m_tabs.size()) - 1;
    m_focusTab = m_active;
    m_open     = true;
    RefreshUsers(ctx, m_tabs.back());
    const Tab& n = m_tabs.back();
    LOG_INFO("[TEXVIEWER] open %s: w=%u h=%u depth=%u mips=%u arraySize=%u cube=%d dimension=%s format=%s "
             "preset=%s srgb=%d bytes=%llu headerDimension=%u headerMismatch=%d map=%s layout=%s tabs=%zu",
             n.path.c_str(), n.info.width, n.info.height, n.info.depth, n.info.mipLevels, n.info.arraySize,
             n.info.isCube ? 1 : 0, n.info.dimension.c_str(), n.info.formatName.c_str(),
             n.info.preset.empty() ? "-" : n.info.preset.c_str(), n.info.isSRGB ? 1 : 0,
             static_cast<unsigned long long>(n.info.dataBytes), n.info.headerDimension,
             n.info.headerMismatch.empty() ? 0 : 1, TextureView::MapName(n.view.map),
             TextureView::CubeLayoutName(n.view.cubeLayout), m_tabs.size());
    return true;
}

bool TextureViewerWindow::CloseTab(const TextureViewerContext& ctx, int tab)
{
    if (tab < 0) tab = m_active;
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size())) return false;
    // How many times this tab's picture was drawn (texviewer_cost: an idle viewer
    // draws once per change, not once per frame).
    std::uint64_t renders = 0;
    if (tab == m_viewTab && m_viewId != ~0u && ctx.renderer)
        if (TextureViewService* views = ctx.renderer->GetTextureViews()) renders = views->GetStatus(m_viewId).renders;
    if (tab == m_viewTab) ReleaseView(ctx);
    if (m_viewTab > tab) --m_viewTab;
    Tab& t = m_tabs[tab];
    // A comparison against this tab ends with it (B's reference is this tab's).
    for (Tab& o : m_tabs)
        if (&o != &t && o.view.compare != TextureView::Compare::Off && o.view.compareWith == t.path)
        {
            o.view.compare = TextureView::Compare::Off;
            o.view.compareWith.clear();
            LOG_INFO("[TEXVIEWER] compare off: %s - its picture B (%s) closed", o.path.c_str(), t.path.c_str());
        }
    if (ctx.textures && ctx.gfx && t.handle != Resource::kInvalidTextureHandle)
        ctx.textures->Release(t.handle, *ctx.gfx);
    LOG_INFO("[TEXVIEWER] close %s: renders=%llu", t.path.c_str(), static_cast<unsigned long long>(renders));
    m_tabs.erase(m_tabs.begin() + tab);
    if (m_active >= static_cast<int>(m_tabs.size())) m_active = static_cast<int>(m_tabs.size()) - 1;
    else if (m_active > tab) --m_active;
    m_focusTab = m_active;
    return true;
}

void TextureViewerWindow::CloseAll(const TextureViewerContext& ctx)
{
    while (!m_tabs.empty()) CloseTab(ctx, static_cast<int>(m_tabs.size()) - 1);
    m_active = -1;
}

bool TextureViewerWindow::Activate(int tab)
{
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size())) return false;
    m_active   = tab;
    m_focusTab = tab;
    return true;
}

void TextureViewerWindow::ReleaseView(const TextureViewerContext& ctx)
{
    if (m_viewId != ~0u && ctx.renderer)
        if (TextureViewService* views = ctx.renderer->GetTextureViews()) views->Release(m_viewId);
    m_viewId  = ~0u;
    m_viewTab = -1;
    m_lastRenders = 0;
    m_lastDescHash = 0;
}

void TextureViewerWindow::ReleaseAtShutdown(const TextureViewerContext& ctx)
{
    CloseAll(ctx);
    ReleaseView(ctx);
}

void TextureViewerWindow::RefreshUsers(const TextureViewerContext& ctx, Tab& t)
{
    t.files.clear();
    if (ctx.referencingFiles) ctx.referencingFiles(t.path, t.files);
    t.entities = 0;
    if (ctx.renderer)
    {
        // The serial window (render lane parked) - TextureConsumers' drain rule.
        std::vector<Entity> users;
        ctx.renderer->TextureConsumers(t.path, users);
        t.entities = static_cast<std::uint32_t>(users.size());
    }
    t.usersKnown = true;
}

// ==== The view ====
void TextureViewerWindow::ImageExtent(const Tab& t, float& w, float& h) const
{
    w = static_cast<float>((std::max)(1u, t.info.width));
    h = static_cast<float>((std::max)(1u, t.info.height));
    if (t.info.isCube && !m_harnessFaceOff)
    {
        if (t.view.cubeLayout == TextureView::CubeLayout::Cross)         { w *= 4.f; h *= 3.f; }
        else if (t.view.cubeLayout == TextureView::CubeLayout::Panorama) { w *= 4.f; h *= 2.f; }
    }
}

void TextureViewerWindow::FitView(Tab& t, float canvasW, float canvasH) const
{
    float iw = 1.f, ih = 1.f;
    ImageExtent(t, iw, ih);
    const float z = (std::max)(kMinZoom, (std::min)(canvasW / iw, canvasH / ih));
    t.view.zoom   = 0.f;   // stays "fit"
    t.view.pan[0] = std::floor((canvasW - iw * z) * 0.5f);
    t.view.pan[1] = std::floor((canvasH - ih * z) * 0.5f);
}

bool TextureViewerWindow::BuildDesc(const TextureViewerContext& ctx, Tab& t, TextureView::Desc& d) const
{
    if (!ctx.textures) return false;
    const RHI::Texture* tex = ctx.textures->GetTexture(t.handle);
    if (!tex) return false;   // not resident yet: nothing to draw
    Resource::TextureSystem::Info ti;
    ctx.textures->Describe(t.handle, ti);

    const std::uint32_t W = m_pinW ? m_pinW : (m_outW ? m_outW : kDefaultOut);
    const std::uint32_t H = m_pinH ? m_pinH : (m_outH ? m_outH : kDefaultOut);
    const float canvasW = m_pinW ? static_cast<float>(m_pinW) : (m_canvasW > 0.f ? m_canvasW : float(W));
    const float canvasH = m_pinH ? static_cast<float>(m_pinH) : (m_canvasH > 0.f ? m_canvasH : float(H));

    float iw = 1.f, ih = 1.f;
    ImageExtent(t, iw, ih);
    float z = t.view.zoom;
    float px = t.view.pan[0], py = t.view.pan[1];
    if (z <= 0.f)
    {
        z  = (std::max)(kMinZoom, (std::min)(canvasW / iw, canvasH / ih));
        px = std::floor((canvasW - iw * z) * 0.5f);
        py = std::floor((canvasH - ih * z) * 0.5f);
    }

    d = TextureView::Desc{};
    d.source         = *tex;
    d.sourceRevision = ti.revision;
    d.width          = W;
    d.height         = H;
    d.mip            = m_harnessMipOff ? 0u : t.view.mip;
    d.slice          = m_harnessFaceOff ? 0u : t.view.slice;
    d.cubeLayout     = m_harnessFaceOff ? TextureView::CubeLayout::Face : t.view.cubeLayout;
    d.channels       = m_harnessChannelOff ? TextureView::kChRGBA : t.view.channels;
    d.map            = m_harnessMapOff ? TextureView::Map::Stored : t.view.map;
    d.exposureEV     = t.view.ev;
    d.tonemap        = t.view.tonemap;
    d.rangeMin       = t.view.rangeMin;
    d.rangeMax       = t.view.rangeMax;
    d.background     = t.view.background;
    d.filtered       = t.view.filtered;
    d.tile           = t.view.tile;
    d.uvOrigin[0]    = -px / (z * iw);
    d.uvOrigin[1]    = -py / (z * ih);
    d.uvPerPixel[0]  = 1.f / (z * iw);
    d.uvPerPixel[1]  = 1.f / (z * ih);

    // Picture B - drawn only once resident (until then A alone).
    std::uint32_t bMip = 0;
    const Tab* b = m_harnessCompareOff ? nullptr : CompareTab(t, bMip);
    if (const RHI::Texture* bt = b ? ctx.textures->GetTexture(b->handle) : nullptr)
    {
        Resource::TextureSystem::Info bi;
        ctx.textures->Describe(b->handle, bi);
        d.compare         = t.view.compare;
        d.compareSource   = *bt;
        d.compareRevision = bi.revision;
        d.compareMip      = bMip;
        d.compareSlice    = d.slice;
        d.splitX          = t.view.split;
        d.diffScale       = t.view.diffScale;
    }
    return true;
}

bool TextureViewerWindow::Settled() const
{
    return m_viewId != ~0u && m_lastDescHash != 0 && m_lastRenders > 0 && m_frame >= m_lastRendersFrame + kSettleFrames &&
           m_lastRendersFrame >= m_descChangedFrame;
}

bool TextureViewerWindow::WriteShot(const TextureViewerContext& ctx, const std::string& path, std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t) { why = "no texture open"; return false; }
    TextureViewService* views = ctx.renderer ? ctx.renderer->GetTextureViews() : nullptr;
    const RHI::Texture* out = (views && m_viewId != ~0u) ? views->Output(m_viewId) : nullptr;
    if (!out || !ctx.gfx) { why = "the picture has no target yet"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(Sys::U8Path(path).parent_path(), ec);
    if (!ctx.gfx->CaptureTextureToPNG(*out, RHI::ResourceState::SHADER_RESOURCE, path.c_str()))
    {
        why = "the capture failed (see the log)";
        return false;
    }
    const TextureView::Status st = views->GetStatus(m_viewId);
    const std::uint32_t face = m_harnessFaceOff ? 0u : t->view.slice;
    std::uint32_t bMip = 0;
    const Tab* b = m_harnessCompareOff ? nullptr : CompareTab(*t, bMip);
    LOG_INFO("[TEXVIEWER] shot %s: file='%s' w=%u h=%u mip=%u face=%u layout=%s ch=%s map=%s ev=%.3f renders=%llu "
             "tab=%s cmp=%s with=%s bmip=%u", BaseName(path).c_str(), path.c_str(), st.width, st.height,
             m_harnessMipOff ? 0u : t->view.mip, face,
             TextureView::CubeLayoutName(m_harnessFaceOff ? TextureView::CubeLayout::Face : t->view.cubeLayout),
             TextureView::ChannelsName(m_harnessChannelOff ? TextureView::kChRGBA : t->view.channels).c_str(),
             TextureView::MapName(t->view.map), t->view.ev, static_cast<unsigned long long>(st.renders),
             t->name.c_str(), TextureView::CompareName(b ? t->view.compare : TextureView::Compare::Off),
             !b ? "-" : (b == t ? "mip" : b->name.c_str()), b ? bMip : 0u);
    return true;
}

// ==== Game half ====
void TextureViewerWindow::ProcessPendingActions(const TextureViewerContext& ctx)
{
    m_frame = ctx.editorFrame;
    TickHarness(ctx);

    // Queued opens / closes (UI handlers).
    for (const std::string& p : m_pendingOpen)
    {
        std::string why;
        if (!OpenNow(ctx, p, why)) LOG_WARNING("[TEXVIEWER] cannot open '%s': %s", p.c_str(), why.c_str());
    }
    m_pendingOpen.clear();
    std::sort(m_pendingClose.begin(), m_pendingClose.end(), std::greater<int>());
    for (int i : m_pendingClose) CloseTab(ctx, i);
    m_pendingClose.clear();

    // A closed window keeps nothing: no view, no texture references.
    if (!m_open)
    {
        if (!m_tabs.empty()) CloseAll(ctx);
        ReleaseView(ctx);
        return;
    }

    TextureViewService* views = ctx.renderer ? ctx.renderer->GetTextureViews() : nullptr;
    Tab* t = ActiveTabPtr();
    if (!t || !views)
    {
        ReleaseView(ctx);
        return;
    }
    // Only the active tab owns a view.
    if (m_viewTab != m_active) ReleaseView(ctx);
    if (m_viewId == ~0u)
    {
        m_viewId  = views->Acquire();
        m_viewTab = m_active;
        m_lastRenders = 0;
        m_lastDescHash = 0;
    }
    if (m_viewId == ~0u) return;

    // A reimport swapped the contents under the same handle: the file says what it
    // is now (a new size, format or mip count), the mip / face stay in range. The
    // first resident revision a tab sees is checked too: a reimport can land before
    // the tab ever saw the texture resident, and then no revision changes under it
    // and the tab would keep describing the old file.
    // DescribeTextureFile is cached by (size, mtime): an unchanged file is a stat.
    if (ctx.textures)
    {
        Resource::TextureSystem::Info ti;
        if (ctx.textures->Describe(t->handle, ti) && ti.gpuReady && ti.revision != t->revision)
        {
            Resource::TextureFileInfo fresh = Resource::DescribeTextureFile(t->path);
            const bool changed = fresh.ok &&
                (t->revision != 0 || fresh.width != t->info.width || fresh.height != t->info.height ||
                 fresh.depth != t->info.depth || fresh.arraySize != t->info.arraySize ||
                 fresh.mipLevels != t->info.mipLevels || fresh.formatName != t->info.formatName);
            if (changed)
            {
                t->info = std::move(fresh);
                t->view.mip = (std::min)(t->view.mip, (std::max)(1u, t->info.mipLevels) - 1u);
                LOG_INFO("[TEXVIEWER] reloaded %s: w=%u h=%u mips=%u format=%s revision=%llu", t->path.c_str(),
                         t->info.width, t->info.height, t->info.mipLevels, t->info.formatName.c_str(),
                         static_cast<unsigned long long>(ti.revision));
                t->importLoaded = false;   // a reimport rewrote its record too
            }
            t->revision = ti.revision;
        }
    }
    // The Import section's record, and the Reimports it asked for.
    if (!t->importLoaded) LoadImport(*t);
    for (const PendingReimport& r : m_pendingReimports)
    {
        std::string why;
        const bool ok = ctx.reimport && ctx.reimport(r.path, r.source, r.settings, why);
        if (!ctx.reimport) why = "no importer in this host";
        for (Tab& tab : m_tabs)
            if (tab.path == r.path)
            {
                tab.importFailed = !ok;
                tab.importStatus = ok ? "Reimported from " + BaseName(r.source) + " as " +
                                        Resource::TextureImporter::GetFormatId(r.settings.format)
                                      : why;
                tab.importLoaded = false;
            }
        LOG_INFO("[TEXVIEWER] reimport %s: ok=%d source='%s' format=%s maxSize=%u mipFilter=%s flipGreen=%d "
                 "mips=%d%s%s", r.path.c_str(), ok ? 1 : 0, r.source.c_str(),
                 Resource::TextureImporter::GetFormatId(r.settings.format), r.settings.maxSize,
                 Resource::TextureImporter::GetMipFilterId(r.settings.mipFilter), r.settings.flipGreen ? 1 : 0,
                 r.settings.generateMips ? 1 : 0, ok ? "" : " why=", ok ? "" : why.c_str());
    }
    m_pendingReimports.clear();

    TextureView::Desc d;
    if (BuildDesc(ctx, *t, d))
    {
        // The reverse arm of texviewer_cost: a description that changes every
        // frame - what an idle viewer would cost if it redrew unconditionally.
        if (m_harnessRedrawAll) d.sourceRevision ^= (m_frame << 32);
        views->Request(m_viewId, d);
        const std::uint64_t h = TextureView::Hash(d);
        if (h != m_lastDescHash) { m_lastDescHash = h; m_descChangedFrame = m_frame; }
    }
    const TextureView::Status st = views->GetStatus(m_viewId);
    if (st.renders != m_lastRenders) { m_lastRenders = st.renders; m_lastRendersFrame = m_frame; }
    if (!t->usersKnown) RefreshUsers(ctx, *t);

    // The probe - a scripted / harness one first (in order), else the hover.
    if (!m_probes.empty())
    {
        const PendingProbe& p = m_probes.front();
        views->RequestProbe(m_viewId, p.x, p.y, p.face, p.sideB);
    }
    else if (m_hoverProbe)
        views->RequestProbe(m_viewId, m_hoverTexX, m_hoverTexY, m_hoverFace, m_hoverSideB);
    else
        views->StopProbe(m_viewId);
    TickProbes(ctx);
    TickStats(ctx);
    m_hoverProbe = false;   // Draw sets it again while the cursor stays on a texel

    // Scripted shots (Editor.TextureViewerShot): written once the picture settles.
    for (std::size_t i = 0; i < m_shots.size();)
    {
        PendingShot& s = m_shots[i];
        if (!Settled() && m_frame <= s.queuedAt + kShotDeadline) { ++i; continue; }
        std::string why;
        if (!Settled())
            Harness::Jobs::Fail(s.job, "timeout", "the picture never settled", {});
        else if (!WriteShot(ctx, s.path, why))
            Harness::Jobs::Fail(s.job, "capture_failed", why, {});
        else
        {
            const TextureView::Status vs = views->GetStatus(m_viewId);
            std::string p = s.path;
            for (char& c : p) if (c == '\\') c = '/';
            Harness::Jobs::Succeed(s.job, "{\"file\":\"" + p + "\",\"w\":" + std::to_string(vs.width) +
                                          ",\"h\":" + std::to_string(vs.height) + ",\"renders\":" +
                                          std::to_string(vs.renders) + "}");
        }
        m_shots.erase(m_shots.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ==== Probes and statistics ====
void TextureViewerWindow::LogProbe(const ProbeReport& r) const
{
    const Tab* t = ActiveTabPtr();
    LOG_INFO("[TEXVIEWER] probe %u,%u: mip=%u face=%u value=%s stored=%s raw=%s block=%d tab=%s side=%s", r.x, r.y,
             r.mip, r.face, r.gpuValid ? Vec4Text(r.gpu).c_str() : "-",
             r.stored.ok ? Vec4Text(r.stored.value.data()).c_str() : "-",
             r.stored.raw.empty() ? "-" : r.stored.raw.c_str(), r.stored.block ? 1 : 0, t ? t->name.c_str() : "-",
             r.sideB ? "b" : "a");
}

void TextureViewerWindow::TickProbes(const TextureViewerContext& ctx)
{
    TextureViewService* views = ctx.renderer ? ctx.renderer->GetTextureViews() : nullptr;
    Tab* t = ActiveTabPtr();
    if (!views || !t || m_viewId == ~0u) return;
    const bool fromQueue = !m_probes.empty();
    if (!fromQueue && !m_hoverProbe) return;
    const TextureViewService::ProbeResult pr = views->GetProbe(m_viewId);
    if (pr.valid)
    {
        m_probe.gpuValid = true;
        for (int c = 0; c < 4; ++c) m_probe.gpu[c] = pr.value[c];
        m_probe.x    = pr.x;
        m_probe.y    = pr.y;
        m_probe.mip  = pr.mip;
        m_probe.face = pr.face;
        m_probe.sideB = pr.sideB;
        // The texture it came from: B's for a probe of the comparison's picture B.
        std::uint32_t bMip = 0;
        const Tab* src = t;
        if (pr.sideB)
            if (const Tab* b = CompareTab(*t, bMip)) src = b;
        m_probe.path = src->path;
        // The stored value: read from the (cached, decoded) file once per texel.
        const bool sliced = src->info.isCube || src->info.arraySize > 1 || src->info.dimension == "3D";
        const std::string key = src->path + "|" + std::to_string(src->revision) + "|" + std::to_string(pr.mip) + "|" +
                                std::to_string(pr.face) + "|" + std::to_string(pr.x) + "|" + std::to_string(pr.y);
        if (key != m_storedKey)
        {
            m_storedKey    = key;
            m_probe.stored = Resource::ReadStoredTexel(src->path, pr.mip, sliced ? pr.face : 0u, pr.x, pr.y);
        }
    }
    if (!fromQueue) return;
    PendingProbe& q = m_probes.front();
    if (pr.valid)
    {
        if (q.harness) LogProbe(m_probe);
        if (q.job)
        {
            const ProbeReport& r = m_probe;
            std::string json = "{\"x\":" + std::to_string(r.x) + ",\"y\":" + std::to_string(r.y) +
                               ",\"mip\":" + std::to_string(r.mip) + ",\"face\":" + std::to_string(r.face) +
                               ",\"side\":\"" + (r.sideB ? "b" : "a") + "\",\"value\":[" + Vec4Text(r.gpu) + "]";
            if (r.stored.ok)
                json += ",\"stored\":[" + Vec4Text(r.stored.value.data()) + "],\"raw\":\"" + r.stored.raw +
                        "\",\"block\":" + (r.stored.block ? "true" : "false");
            json += "}";
            Harness::Jobs::Succeed(q.job, json);
        }
        m_probes.erase(m_probes.begin());
        return;
    }
    if (m_frame > q.queuedAt + kShotDeadline)
    {
        if (q.job) Harness::Jobs::Fail(q.job, "timeout", "no probe value came back", {});
        if (q.harness)
        {
            m_harnessFailed = true;
            LOG_ERROR("[TEXVIEWER] probe %u,%u never came back", q.x, q.y);
        }
        m_probes.erase(m_probes.begin());
    }
}

std::uint64_t TextureViewerWindow::QueueProbe(std::uint32_t x, std::uint32_t y, int face, std::uint32_t requestId,
                                              bool sideB)
{
    const std::uint64_t job = Harness::Jobs::TryCreate("texviewer_probe", std::to_string(x) + "," + std::to_string(y),
                                                    requestId);
    if (!job) return 0;
    Harness::Jobs::Start(job);
    PendingProbe p;
    p.x = x; p.y = y; p.face = face; p.job = job; p.queuedAt = m_frame; p.sideB = sideB;
    m_probes.push_back(p);
    return job;
}

void TextureViewerWindow::StartStats(const Resource::AnalyzeOptions& o, std::uint64_t job)
{
    const Tab* t = ActiveTabPtr();
    if (!t) return;
    if (m_statsTask)
    {
        // One at a time: a request made while another runs waits its turn.
        m_statsQueue.push_back({ o, job });
        return;
    }
    auto task      = std::make_shared<StatsTask>();
    task->path     = t->path;
    task->revision = t->revision;
    task->options  = o;
    if (job) task->jobs.push_back(job);
    m_statsTask = task;
    TaskSystem::Get().Push([task]()
    {
        try { task->result = Resource::AnalyzeTexture(task->path, task->options); }
        catch (const std::exception& e) { task->result = {}; task->result.error = e.what(); }
        catch (...) { task->result = {}; task->result.error = "the analysis threw"; }
        task->finished.store(true, std::memory_order_release);
    }, TaskSystem::TaskPriority::Low);
}

std::string TextureViewerWindow::StatsJson(const Resource::TextureStats& s)
{
    auto u4 = [](const std::array<std::uint64_t, 4>& v)
    {
        return std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) + "," + std::to_string(v[3]);
    };
    std::string json = "{\"mip\":" + std::to_string(s.mip) + ",\"slice\":" + std::to_string(s.slice) +
                       ",\"width\":" + std::to_string(s.width) + ",\"height\":" + std::to_string(s.height) +
                       ",\"texels\":" + std::to_string(s.texels) + ",\"channels\":" + std::to_string(s.channels) +
                       ",\"min\":[" + Vec4Text(s.min) + "],\"max\":[" + Vec4Text(s.max) +
                       "],\"mean\":[" + Vec4Text(s.mean) + "],\"stddev\":[" + Vec4Text(s.stddev) +
                       "],\"nan\":[" + u4(s.nan) + "],\"inf\":[" + u4(s.inf) + "],\"alpha\":\"" + s.alpha +
                       "\",\"monochrome\":" + (s.monochrome ? "true" : "false") + ",\"ms\":" + std::to_string(s.ms);
    if (s.normalChecked)
        json += ",\"normal\":{\"outsideDisc\":" + std::to_string(s.normalOutsideDisc) + ",\"lengthDev\":" +
                std::to_string(s.normalLengthDev) + ",\"zNegative\":" + std::to_string(s.normalZNegative) + "}";
    if (!s.mipMeans.empty())
        json += ",\"mipMaxDrift\":" + std::to_string(s.mipMaxDrift) + ",\"mipCount\":" + std::to_string(s.mipMeans.size());
    json += "}";
    return json;
}

void TextureViewerWindow::TickStats(const TextureViewerContext& ctx)
{
    (void)ctx;
    const Tab* t = ActiveTabPtr();
    if (m_statsTask && m_statsTask->finished.load(std::memory_order_acquire))
    {
        std::shared_ptr<StatsTask> done = std::move(m_statsTask);
        m_statsTask.reset();
        const Resource::TextureStats& s = done->result;
        for (std::uint64_t job : done->jobs)
        {
            if (!s.ok) Harness::Jobs::Fail(job, "analysis_failed", s.error, {});
            else       Harness::Jobs::Succeed(job, StatsJson(s));
        }
        if (done->harness)
        {
            if (!s.ok)
            {
                m_harnessFailed = true;
                LOG_ERROR("[TEXVIEWER] stats failed: %s", s.error.c_str());
            }
            else
                LOG_INFO("[TEXVIEWER] stats mip%u_face%u: min=%s max=%s mean=%s nanR=%llu infR=%llu nan=%llu inf=%llu "
                         "alpha=%s mono=%d texels=%llu ms=%.1f", s.mip, s.slice, Vec4Text(s.min).c_str(),
                         Vec4Text(s.max).c_str(), Vec4Text(s.mean).c_str(),
                         static_cast<unsigned long long>(s.nan[0]), static_cast<unsigned long long>(s.inf[0]),
                         static_cast<unsigned long long>(s.nan[0] + s.nan[1] + s.nan[2] + s.nan[3]),
                         static_cast<unsigned long long>(s.inf[0] + s.inf[1] + s.inf[2] + s.inf[3]), s.alpha.c_str(),
                         s.monochrome ? 1 : 0, static_cast<unsigned long long>(s.texels), s.ms);
        }
        // Only a result for the tab / contents it was asked about is shown.
        if (t && done->path == t->path && done->revision == t->revision) m_statsDone = std::move(done);
    }
    if (!m_statsTask && !m_statsQueue.empty())
    {
        const auto next = m_statsQueue.front();
        m_statsQueue.erase(m_statsQueue.begin());
        StartStats(next.first, next.second);
        if (next.second == 0 && m_statsTask) m_statsTask->harness = true;   // a harness request carries no job
        return;
    }
    // Automatic: the shown (mip, slice) of the active tab, once per change.
    if (m_statsAuto && t && !m_statsTask && t->revision != 0)
    {
        const bool sliced = t->info.isCube || t->info.arraySize > 1 || t->info.dimension == "3D";
        const std::uint32_t face = sliced ? t->view.slice : 0u;
        const bool have = m_statsDone && m_statsDone->path == t->path && m_statsDone->revision == t->revision &&
                          m_statsDone->options.mip == t->view.mip && m_statsDone->options.slice == face;
        if (!have)
        {
            Resource::AnalyzeOptions o;
            o.mip         = t->view.mip;
            o.slice       = face;
            o.normalCheck = t->view.map == TextureView::Map::Normal;
            StartStats(o, 0);
        }
    }
}

std::uint64_t TextureViewerWindow::QueueStats(const Resource::AnalyzeOptions& o, std::uint32_t requestId)
{
    const std::uint64_t job = Harness::Jobs::TryCreate("texviewer_stats", "mip " + std::to_string(o.mip), requestId);
    if (!job) return 0;
    Harness::Jobs::Start(job);
    StartStats(o, job);
    return job;
}

const Resource::TextureStats* TextureViewerWindow::Stats() const
{
    const Tab* t = ActiveTabPtr();
    if (!t || !m_statsDone || m_statsDone->path != t->path || !m_statsDone->result.ok) return nullptr;
    return &m_statsDone->result;
}

std::uint64_t TextureViewerWindow::QueueShot(const std::string& path, std::uint32_t requestId)
{
    const std::uint64_t job = Harness::Jobs::TryCreate("texviewer_shot", path, requestId);
    if (!job) return 0;
    Harness::Jobs::Start(job);
    m_shots.push_back({ path, job, m_frame });
    return job;
}

TextureViewerWindow::StateView TextureViewerWindow::State(const TextureViewerContext& ctx) const
{
    StateView s;
    s.open   = m_open;
    s.active = m_active;
    TextureViewService* views = ctx.renderer ? ctx.renderer->GetTextureViews() : nullptr;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
    {
        const Tab& t = m_tabs[i];
        TabState ts;
        ts.path = t.path;
        ts.view = t.view;
        ts.info = t.info;
        if (ctx.textures && ctx.resources)
        {
            const Resource::TextureSystem::Status st = ctx.textures->GetStatus(t.handle, *ctx.resources);
            ts.resident    = st.state == Resource::TextureSystem::Status::State::Ready;
            ts.residentWhy = st.reason;
            Resource::TextureSystem::Info ti;
            if (ctx.textures->Describe(t.handle, ti)) ts.gpuBytes = ti.gpuBytes;
        }
        if (i == m_viewTab && views && m_viewId != ~0u)
        {
            const TextureView::Status vs = views->GetStatus(m_viewId);
            ts.viewId  = m_viewId;
            ts.renders = vs.renders;
            ts.outW    = vs.width;
            ts.outH    = vs.height;
            ts.settled = Settled();
        }
        s.tabs.push_back(std::move(ts));
    }
    return s;
}

// ==== The scripted surface ====
bool TextureViewerWindow::Set(const std::string& keyIn, const std::string& value, std::string& why)
{
    Tab* t = ActiveTabPtr();
    const std::string key = Lower(keyIn);
    if (key == "size")
    {
        std::uint32_t w = 0, h = 0;
        if (sscanf_s(value.c_str(), "%ux%u", &w, &h) != 2 || w > TextureViewService::kMaxSize ||
            h > TextureViewService::kMaxSize || (w == 0) != (h == 0))
        {
            why = "size is <w>x<h> (1..4096, or 0x0 = follow the canvas)";
            return false;
        }
        PinSize(w, h);
        return true;
    }
    if (!t) { why = "no texture open"; return false; }
    ViewState v = t->view;
    if (key == "ch" || key == "channels")
    {
        if (!TextureView::ParseChannels(value, v.channels)) { why = "ch is some of r, g, b, a (\"rgba\", \"r\", ...)"; return false; }
    }
    else if (key == "mip")
    {
        std::uint32_t m = 0;
        if (!ParseUInt(value, m) || m >= (std::max)(1u, t->info.mipLevels))
        {
            why = "mip is 0.." + std::to_string((std::max)(1u, t->info.mipLevels) - 1);
            return false;
        }
        v.mip = m;
    }
    else if (key == "face" || key == "slice")
    {
        TextureView::CubeLayout l;
        if (key == "face" && TextureView::ParseCubeLayout(value, l) && l != TextureView::CubeLayout::Face)
            v.cubeLayout = l;
        else
        {
            std::uint32_t f = 0;
            const std::uint32_t count = t->info.isCube ? 6u : (t->info.dimension == "3D" ? t->info.depth : t->info.arraySize);
            if (!ParseUInt(value, f) || f >= (std::max)(1u, count))
            {
                why = key + " is 0.." + std::to_string((std::max)(1u, count) - 1) + (t->info.isCube ? ", cross or pano" : "");
                return false;
            }
            v.slice = f;
            if (t->info.isCube) v.cubeLayout = TextureView::CubeLayout::Face;
        }
    }
    else if (key == "layout")
    {
        if (!TextureView::ParseCubeLayout(value, v.cubeLayout)) { why = "layout is face, cross or pano"; return false; }
    }
    else if (key == "map")
    {
        // map=range:a:b sets the range too.
        std::string m = value;
        const std::size_t c = m.find(':');
        if (c != std::string::npos)
        {
            float a = 0.f, b = 0.f;
            const std::size_t c2 = m.find(':', c + 1);
            if (c2 == std::string::npos || !ParseFloat(m.substr(c + 1, c2 - c - 1), a) || !ParseFloat(m.substr(c2 + 1), b))
            {
                why = "map=range:<min>:<max>";
                return false;
            }
            v.rangeMin = a;
            v.rangeMax = b;
            m = m.substr(0, c);
        }
        if (!TextureView::ParseMap(m, v.map)) { why = "map is stored, linear, hdr, normal or range[:a:b]"; return false; }
    }
    else if (key == "ev")
    {
        if (!ParseFloat(value, v.ev)) { why = "ev is a number"; return false; }
    }
    else if (key == "tonemap")
    {
        if (!ParseBool(value, v.tonemap)) { why = "tonemap is 0 or 1"; return false; }
    }
    else if (key == "range")
    {
        const std::size_t c = value.find(':');
        if (c == std::string::npos || !ParseFloat(value.substr(0, c), v.rangeMin) ||
            !ParseFloat(value.substr(c + 1), v.rangeMax))
        {
            why = "range is <min>:<max>";
            return false;
        }
    }
    else if (key == "bg" || key == "background")
    {
        if (!TextureView::ParseBackground(value, v.background)) { why = "bg is checker, black, grey or white"; return false; }
    }
    else if (key == "filtered" || key == "tile")
    {
        bool b = false;
        if (!ParseBool(value, b)) { why = key + " is 0 or 1"; return false; }
        (key == "filtered" ? v.filtered : v.tile) = b;
    }
    else if (key == "zoom")
    {
        if (Lower(value) == "fit") v.zoom = 0.f;
        else
        {
            float z = 0.f;
            if (!ParseFloat(value, z) || z < kMinZoom || z > kMaxZoom) { why = "zoom is fit or 1/256..256"; return false; }
            // A number keeps the picture's centre where it is.
            const float cw = m_pinW ? float(m_pinW) : (m_canvasW > 0.f ? m_canvasW : float(kDefaultOut));
            const float ch = m_pinH ? float(m_pinH) : (m_canvasH > 0.f ? m_canvasH : float(kDefaultOut));
            float iw = 1.f, ih = 1.f;
            ImageExtent(*t, iw, ih);
            v.zoom   = z;
            v.pan[0] = std::floor((cw - iw * z) * 0.5f);
            v.pan[1] = std::floor((ch - ih * z) * 0.5f);
        }
    }
    else if (key == "pan")
    {
        const std::size_t c = value.find(':');
        if (c == std::string::npos || !ParseFloat(value.substr(0, c), v.pan[0]) || !ParseFloat(value.substr(c + 1), v.pan[1]))
        {
            why = "pan is <x>:<y> (the picture's top-left, output pixels)";
            return false;
        }
        if (v.zoom <= 0.f)
        {
            // Panning a fitted picture fixes its zoom where it is.
            const float cw = m_pinW ? float(m_pinW) : (m_canvasW > 0.f ? m_canvasW : float(kDefaultOut));
            const float ch = m_pinH ? float(m_pinH) : (m_canvasH > 0.f ? m_canvasH : float(kDefaultOut));
            float iw = 1.f, ih = 1.f;
            ImageExtent(*t, iw, ih);
            v.zoom = (std::max)(kMinZoom, (std::min)(cw / iw, ch / ih));
        }
    }
    else if (key == "cmp" || key == "compare")
    {
        if (!TextureView::ParseCompare(value, v.compare)) { why = "cmp is off, split or diff"; return false; }
    }
    else if (key == "with")
    {
        // Picture B: "mip" = this texture's next mip, else another open tab -
        // its number (1-based, as Editor.TextureViewer lists them) or its path.
        const std::string l = Lower(value);
        if (l.empty() || l == "mip" || l == "next")
            v.compareWith.clear();
        else
        {
            int idx = -1;
            std::uint32_t n = 0;
            if (ParseUInt(value, n))
                idx = static_cast<int>(n) - 1;
            else
            {
                std::string p = Sys::RelativizeAssetPath(value);
                for (char& c : p) if (c == '\\') c = '/';
                for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
                    if (Lower(m_tabs[i].path) == Lower(p)) idx = i;
            }
            if (idx < 0 || idx >= static_cast<int>(m_tabs.size()))
            {
                why = "with is mip, or an open tab's number (1-based) or path";
                return false;
            }
            const Tab& b = m_tabs[static_cast<std::size_t>(idx)];
            if (&b == t) { why = "with names this tab itself (with=mip compares its next mip)"; return false; }
            if (b.info.isCube != t->info.isCube) { why = "A and B must both be cubes, or neither"; return false; }
            v.compareWith = b.path;
        }
    }
    else if (key == "split")
    {
        float f = 0.f;
        if (!ParseFloat(value, f) || f < 0.f || f > 1.f) { why = "split is 0..1 (where B starts)"; return false; }
        v.split = f;
    }
    else if (key == "k" || key == "diffscale")
    {
        float f = 0.f;
        if (!ParseFloat(value, f) || f <= 0.f || f > 1.0e6f) { why = "k is a number > 0 (Diff: |A - B| x k)"; return false; }
        v.diffScale = f;
    }
    else
    {
        why = "unknown key '" + keyIn + "' (ch, mip, face, slice, layout, map, ev, tonemap, range, bg, filtered, tile, "
              "zoom, pan, size, cmp, with, split, k)";
        return false;
    }
    t->view = v;
    return true;
}

bool TextureViewerWindow::ApplySpec(const std::string& spec, std::string& why)
{
    // All or nothing: apply to a copy of the tab, then commit.
    Tab* t = ActiveTabPtr();
    const ViewState before = t ? t->view : ViewState{};
    const std::uint32_t pw = m_pinW, ph = m_pinH;
    for (const std::string& kv : SplitList(spec, ';'))
    {
        const std::size_t e = kv.find('=');
        if (e == std::string::npos || e == 0)
        {
            why = "'" + kv + "' is not key=value";
            if (t) t->view = before;
            m_pinW = pw; m_pinH = ph;
            return false;
        }
        if (!Set(kv.substr(0, e), kv.substr(e + 1), why))
        {
            if (t) t->view = before;
            m_pinW = pw; m_pinH = ph;
            return false;
        }
    }
    return true;
}

// ==== UI ====
void TextureViewerWindow::DrawToolbar(Tab& t)
{
    ViewState& v = t.view;
    // Channels: click toggles, Shift+click solos.
    static const char* kCh[4] = { "R", "G", "B", "A" };
    for (int c = 0; c < 4; ++c)
    {
        const std::uint32_t bit = 1u << c;
        const bool on = (v.channels & bit) != 0;
        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_Button, ChannelColor(c, on));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ChannelColor(c, true));
        if (ImGui::Button(kCh[c], ImVec2(24.f, 0.f)))
        {
            if (ImGui::GetIO().KeyShift) v.channels = bit;
            else v.channels ^= bit;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s channel (Shift+click: this one alone)", kCh[c]);
        ImGui::PopID();
        ImGui::SameLine(0.f, 2.f);
    }
    ImGui::SameLine(0.f, 10.f);

    // Mip.
    const std::uint32_t mips = (std::max)(1u, t.info.mipLevels);
    ImGui::SetNextItemWidth(150.f);
    char mipLabel[64];
    std::snprintf(mipLabel, sizeof(mipLabel), "Mip %u  %ux%u", v.mip, (std::max)(1u, t.info.width >> v.mip),
                  (std::max)(1u, t.info.height >> v.mip));
    if (ImGui::BeginCombo("##mip", mipLabel))
    {
        for (std::uint32_t m = 0; m < mips; ++m)
        {
            char l[64];
            std::snprintf(l, sizeof(l), "Mip %u  %ux%u", m, (std::max)(1u, t.info.width >> m), (std::max)(1u, t.info.height >> m));
            if (ImGui::Selectable(l, m == v.mip)) v.mip = m;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The mip shown ( [ and ] step it )");

    // Cube: layout and face; an array / 3D texture: the slice.
    if (t.info.isCube)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        int layout = static_cast<int>(v.cubeLayout);
        if (ImGui::Combo("##layout", &layout, "Face\0Cross\0Panorama\0"))
            v.cubeLayout = static_cast<TextureView::CubeLayout>(layout);
        if (v.cubeLayout == TextureView::CubeLayout::Face)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            int face = static_cast<int>((std::min)(v.slice, 5u));
            if (ImGui::Combo("##face", &face, "+X\0-X\0+Y\0-Y\0+Z\0-Z\0")) v.slice = static_cast<std::uint32_t>(face);
        }
    }
    else if (t.info.arraySize > 1 || t.info.dimension == "3D")
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.f);
        const int count = static_cast<int>(t.info.dimension == "3D" ? (std::max)(1u, t.info.depth >> v.mip) : t.info.arraySize);
        int s = static_cast<int>(v.slice);
        if (ImGui::SliderInt("##slice", &s, 0, (std::max)(0, count - 1), "Slice %d")) v.slice = static_cast<std::uint32_t>(s);
    }

    // Mapping.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    int map = static_cast<int>(v.map);
    if (ImGui::Combo("##map", &map, "Stored\0Linear\0HDR\0Normal\0Range\0")) v.map = static_cast<TextureView::Map>(map);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stored: the bytes as stored (an sRGB texture shows its gamma values)\n"
                          "Linear: what the shaders read (an sRGB texture looks darker)\n"
                          "HDR: exposure + tonemap\nNormal: RG with Z rebuilt\nRange: [min, max] -> [0, 1]");
    if (v.map == TextureView::Map::Hdr)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        ImGui::DragFloat("##ev", &v.ev, 0.05f, -16.f, 16.f, "EV %+.2f");
        ImGui::SameLine();
        ImGui::Checkbox("Tonemap", &v.tonemap);
    }
    else if (v.map == TextureView::Map::Range)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.f);
        float r[2] = { v.rangeMin, v.rangeMax };
        if (ImGui::DragFloat2("##range", r, 0.005f, -1.0e6f, 1.0e6f, "%.3f")) { v.rangeMin = r[0]; v.rangeMax = r[1]; }
    }

    // Background, sampling, tiling, zoom.
    ImGui::SameLine(0.f, 10.f);
    ImGui::SetNextItemWidth(80.f);
    int bg = static_cast<int>(v.background);
    if (ImGui::Combo("##bg", &bg, "Checker\0Black\0Grey\0White\0")) v.background = static_cast<TextureView::Background>(bg);
    ImGui::SameLine();
    ImGui::Checkbox("Filtered", &v.filtered);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bilinear inside the mip (for a zoomed-out look); off = exact texels");
    ImGui::SameLine();
    ImGui::Checkbox("Tile", &v.tile);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the texture (see its seams)");

    // Compare with a picture B.
    ImGui::SameLine(0.f, 10.f);
    ImGui::SetNextItemWidth(70.f);
    int cmp = static_cast<int>(v.compare);
    if (ImGui::Combo("##cmp", &cmp, "No B\0Split\0Diff\0")) v.compare = static_cast<TextureView::Compare>(cmp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Compare with a picture B (the next mip, or another open tab):\n"
                          "Split: A left of a line you drag, B right of it\nDiff: |A - B| x k");
    if (v.compare != TextureView::Compare::Off)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.f);
        const std::string cur = v.compareWith.empty() ? "B: next mip" : "B: " + BaseName(v.compareWith);
        if (ImGui::BeginCombo("##cmpwith", cur.c_str()))
        {
            if (ImGui::Selectable("Next mip (N+1)", v.compareWith.empty())) v.compareWith.clear();
            for (const Tab& o : m_tabs)
            {
                if (&o == &t) continue;
                const bool usable = o.info.isCube == t.info.isCube;
                if (!usable) ImGui::BeginDisabled();
                if (ImGui::Selectable((o.name + "##cmp" + o.path).c_str(), v.compareWith == o.path)) v.compareWith = o.path;
                if (!usable) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (v.compare == TextureView::Compare::Diff)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.f);
            ImGui::DragFloat("##k", &v.diffScale, 0.02f, 0.01f, 1000.f, "x%.2f", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("k: the difference is shown x k");
        }
    }
    ImGui::SameLine(0.f, 10.f);
    if (ImGui::Button("Fit")) { v.zoom = 0.f; t.userMoved = false; }
    ImGui::SameLine();
    if (ImGui::Button("1:1"))
    {
        float iw = 1.f, ih = 1.f;
        ImageExtent(t, iw, ih);
        v.zoom   = 1.f;
        v.pan[0] = std::floor((m_canvasW - iw) * 0.5f);
        v.pan[1] = std::floor((m_canvasH - ih) * 0.5f);
        t.userMoved = true;
    }
}

void TextureViewerWindow::DrawCanvas(const TextureViewerContext& ctx, Tab& t, float width, float height)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(24, 24, 24, 255));

    // The output follows the canvas: quantised up to 16 px (the target always
    // covers it, drawn 1:1) and debounced 200 ms. A pinned size wins.
    m_canvasW = width;
    m_canvasH = height;
    const std::uint32_t qw = (std::max)(16u, (static_cast<std::uint32_t>(std::ceil(width)) + 15u) & ~15u);
    const std::uint32_t qh = (std::max)(16u, (static_cast<std::uint32_t>(std::ceil(height)) + 15u) & ~15u);
    if (qw != m_outW || qh != m_outH)
    {
        const float now = static_cast<float>(ImGui::GetTime());
        if (m_outW == 0 || m_sizeChangedAt < 0.f) m_sizeChangedAt = now;
        if (m_outW == 0 || now - m_sizeChangedAt >= 0.2f)
        {
            m_outW = qw;
            m_outH = qh;
            m_sizeChangedAt = -1.f;
        }
    }

    ImGui::InvisibleButton("##tvcanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    // The split line - where picture B starts - is dragged, not the picture.
    TextureViewService* views = ctx.renderer ? ctx.renderer->GetTextureViews() : nullptr;
    const TextureView::Status st = (views && m_viewId != ~0u && m_viewTab == m_active) ? views->GetStatus(m_viewId)
                                                                                         : TextureView::Status{};
    const float outW = st.width ? static_cast<float>(st.width) : width;
    std::uint32_t bMip = 0;
    const Tab* bTab = CompareTab(t, bMip);
    const bool splitOn = bTab && t.view.compare == TextureView::Compare::Split;
    const float splitX = origin.x + t.view.split * outW;
    const bool nearSplit = splitOn && hovered && std::fabs(io.MousePos.x - splitX) <= 5.f;
    if (ImGui::IsItemActivated() && nearSplit && ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_splitDrag = true;
    if (!active || !splitOn) m_splitDrag = false;
    if (nearSplit || m_splitDrag) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (m_splitDrag) t.view.split = std::clamp((io.MousePos.x - origin.x) / outW, 0.f, 1.f);

    // The effective zoom / pan (a fitted picture recomputes them every frame).
    float iw = 1.f, ih = 1.f;
    ImageExtent(t, iw, ih);
    if (t.view.zoom <= 0.f) FitView(t, width, height);
    float z = t.view.zoom > 0.f ? t.view.zoom : (std::max)(kMinZoom, (std::min)(width / iw, height / ih));

    if (hovered && io.MouseWheel != 0.f)
    {
        const float nz = std::clamp(z * std::pow(1.25f, io.MouseWheel), kMinZoom, kMaxZoom);
        const float cx = io.MousePos.x - origin.x, cy = io.MousePos.y - origin.y;
        // Keep the image point under the cursor where it is.
        t.view.pan[0] = cx - (cx - t.view.pan[0]) * (nz / z);
        t.view.pan[1] = cy - (cy - t.view.pan[1]) * (nz / z);
        t.view.zoom   = nz;
        z = nz;
        t.userMoved = true;
    }
    if (active && !m_splitDrag &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)))
    {
        if (t.view.zoom <= 0.f) t.view.zoom = z;
        t.view.pan[0] += io.MouseDelta.x;
        t.view.pan[1] += io.MouseDelta.y;
        t.userMoved = true;
    }

    const std::uint64_t srv = (views && m_viewId != ~0u && m_viewTab == m_active) ? views->Srv(m_viewId) : 0;
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    if (srv)
    {
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(srv)), origin,
                     ImVec2(origin.x + static_cast<float>(st.width), origin.y + static_cast<float>(st.height)));
        if (!st.why.empty())
            dl->AddText(ImVec2(origin.x + 8.f, origin.y + 8.f), IM_COL32(255, 180, 80, 255), st.why.c_str());
    }
    else
    {
        const char* msg = (ctx.textures && ctx.textures->GetTexture(t.handle)) ? "Preparing the view..." : "Loading...";
        dl->AddText(ImVec2(origin.x + 8.f, origin.y + 8.f), IM_COL32(200, 200, 200, 255), msg);
    }

    // Pixel grid once a shown texel is 8 px or more (2D, not tiled).
    const float texelPx = z * static_cast<float>(1u << (std::min)(t.view.mip, 31u));
    const bool  flat    = !t.info.isCube || t.view.cubeLayout == TextureView::CubeLayout::Face;
    if (srv && flat && !t.view.tile && texelPx >= 8.f)
    {
        const float x0 = origin.x + t.view.pan[0], y0 = origin.y + t.view.pan[1];
        const float x1 = x0 + iw * z, y1 = y0 + ih * z;
        const ImU32 col = IM_COL32(0, 0, 0, 90);
        const float sx = (std::max)(origin.x, x0), ex = (std::min)(origin.x + width, x1);
        const float sy = (std::max)(origin.y, y0), ey = (std::min)(origin.y + height, y1);
        for (float x = x0 + std::ceil((sx - x0) / texelPx) * texelPx; x <= ex; x += texelPx)
            dl->AddLine(ImVec2(x, sy), ImVec2(x, ey), col);
        for (float y = y0 + std::ceil((sy - y0) / texelPx) * texelPx; y <= ey; y += texelPx)
            dl->AddLine(ImVec2(sx, y), ImVec2(ex, y), col);
    }
    if (srv && splitOn)
    {
        const ImU32 line = (nearSplit || m_splitDrag) ? IM_COL32(255, 220, 90, 255) : IM_COL32(255, 255, 255, 190);
        dl->AddLine(ImVec2(splitX, origin.y), ImVec2(splitX, origin.y + height), line, 1.f);
        dl->AddText(ImVec2(splitX - 16.f, origin.y + 6.f), line, "A");
        dl->AddText(ImVec2(splitX + 7.f, origin.y + 6.f), line, "B");
    }
    dl->PopClipRect();

    // The texel under the cursor (the status line).
    m_hoverX = m_hoverY = -1;
    m_hoverProbe = false;
    m_hoverSideB = false;
    if (hovered && !m_splitDrag)
    {
        const float u = (io.MousePos.x - origin.x - t.view.pan[0]) / (z * iw);
        const float vv = (io.MousePos.y - origin.y - t.view.pan[1]) / (z * ih);
        m_hoverU = u;
        m_hoverV = vv;
        // Right of the split the texel is picture B's, in B's shown mip.
        m_hoverSideB = splitOn && io.MousePos.x >= splitX;
        const Tab&          src    = m_hoverSideB ? *bTab : t;
        const std::uint32_t srcMip = m_hoverSideB ? bMip : t.view.mip;
        const std::uint32_t mw = (std::max)(1u, src.info.width >> srcMip);
        const std::uint32_t mh = (std::max)(1u, src.info.height >> srcMip);
        const bool cross = t.info.isCube && t.view.cubeLayout == TextureView::CubeLayout::Cross;
        const bool pano  = t.info.isCube && t.view.cubeLayout == TextureView::CubeLayout::Panorama;
        float fu = u, fv = vv;
        int   face = -1;
        bool  inside = false;
        if (cross)
        {
            if (u >= 0.f && u < 1.f && vv >= 0.f && vv < 1.f)
            {
                // TextureView.cs.hlsl's cross: +Y over -X +Z +X -Z over -Y.
                const int cx = static_cast<int>(u * 4.f), cy = static_cast<int>(vv * 3.f);
                if (cy == 0 && cx == 1) face = 2;
                else if (cy == 2 && cx == 1) face = 3;
                else if (cy == 1) face = cx == 0 ? 1 : cx == 1 ? 4 : cx == 2 ? 0 : 5;
                fu = u * 4.f - static_cast<float>(cx);
                fv = vv * 3.f - static_cast<float>(cy);
                inside = face >= 0;
            }
        }
        else if (!pano)
        {
            if (t.view.tile) { fu -= std::floor(fu); fv -= std::floor(fv); inside = true; }
            else inside = u >= 0.f && u < 1.f && vv >= 0.f && vv < 1.f;
        }
        if (inside)
        {
            m_hoverX = (std::min)(static_cast<int>(fu * mw), static_cast<int>(mw) - 1);
            m_hoverY = (std::min)(static_cast<int>(fv * mh), static_cast<int>(mh) - 1);
            m_hoverProbe = true;
            m_hoverTexX  = static_cast<std::uint32_t>(m_hoverX);
            m_hoverTexY  = static_cast<std::uint32_t>(m_hoverY);
            m_hoverFace  = face;
        }
    }
}

void TextureViewerWindow::DrawStats(Tab& t)
{
    const Resource::TextureStats* s = Stats();
    const bool running = m_statsTask != nullptr;
    if (!s)
    {
        ImGui::TextDisabled("%s", running ? "Computing..." : "-");
        return;
    }
    ImGui::TextDisabled("mip %u  slice %u  %ux%u  %.0f ms%s", s->mip, s->slice, s->width, s->height, s->ms,
                        running ? "  (updating)" : "");
    static const char* kCh[4] = { "R", "G", "B", "A" };
    const std::uint32_t shown = (std::max)(s->channels, s->hasAlpha ? 4u : s->channels);
    if (ImGui::BeginTable("##stats", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Min");
        ImGui::TableSetupColumn("Max");
        ImGui::TableSetupColumn("Mean");
        ImGui::TableSetupColumn("StdDev");
        ImGui::TableHeadersRow();
        for (std::uint32_t c = 0; c < shown && c < 4; ++c)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(kCh[c]);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.4g", s->min[c]);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.4g", s->max[c]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.4g", s->mean[c]);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.4g", s->stddev[c]);
        }
        ImGui::EndTable();
    }
    const std::uint64_t nan = s->nan[0] + s->nan[1] + s->nan[2] + s->nan[3];
    const std::uint64_t inf = s->inf[0] + s->inf[1] + s->inf[2] + s->inf[3];
    if (nan || inf)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 1.f, 1.f), "NaN %llu  Inf %llu (shown magenta)",
                           static_cast<unsigned long long>(nan), static_cast<unsigned long long>(inf));
    ImGui::TextDisabled("Alpha");
    ImGui::SameLine(96.f);
    ImGui::TextUnformatted(s->alpha.c_str());
    if (s->monochrome)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "  one colour (a placeholder?)");
    }
    if (s->normalChecked)
    {
        ImGui::TextDisabled("Normal");
        ImGui::SameLine(96.f);
        if (s->channels >= 3)
            ImGui::Text("|n|-1 %.4f  z<0 %.2f%%  outside %.2f%%", s->normalLengthDev, s->normalZNegative * 100.0,
                        s->normalOutsideDisc * 100.0);
        else
            ImGui::Text("RG outside the unit disc %.2f%%", s->normalOutsideDisc * 100.0);
    }
    if (!s->mipMeans.empty())
    {
        ImGui::TextDisabled("Mip chain");
        ImGui::SameLine(96.f);
        ImGui::Text("max drift %.4f over %zu mips", s->mipMaxDrift, s->mipMeans.size());
    }
    else if (t.info.mipLevels > 1 && ImGui::SmallButton("Check the mip chain"))
    {
        Resource::AnalyzeOptions o;
        o.mip      = s->mip;
        o.slice    = s->slice;
        o.mipChain = true;
        StartStats(o, 0);
    }

    // The histogram: 256 bins per channel over each channel's range; drag across
    // it to show that value range (the Range mapping).
    ImGui::Checkbox("Log", &m_histLog);
    ImGui::SameLine();
    ImGui::TextDisabled("[%.3g .. %.3g]  drag: show a range", s->histLo[0], s->histHi[0]);
    const float  w  = (std::max)(64.f, ImGui::GetContentRegionAvail().x);
    const float  h  = 90.f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hist", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 20, 20, 255));
    for (std::uint32_t c = 0; c < shown && c < 4; ++c)
    {
        double peak = 0.0;
        for (std::uint32_t b = 0; b < 256; ++b)
        {
            const double v = m_histLog ? std::log1p(double(s->histogram[c][b])) : double(s->histogram[c][b]);
            peak = (std::max)(peak, v);
        }
        if (peak <= 0.0) continue;
        ImVec2 prev;
        for (std::uint32_t b = 0; b < 256; ++b)
        {
            const double v = m_histLog ? std::log1p(double(s->histogram[c][b])) : double(s->histogram[c][b]);
            const ImVec2 pt(p0.x + (b + 0.5f) * w / 256.f, p0.y + h - static_cast<float>(v / peak) * (h - 2.f));
            if (b) dl->AddLine(prev, pt, ChannelColor(static_cast<int>(c), true), 1.f);
            prev = pt;
        }
    }
    if (ImGui::IsItemActive())
    {
        const float mx = std::clamp(ImGui::GetIO().MousePos.x - p0.x, 0.f, w);
        const int bin = std::clamp(static_cast<int>(mx / w * 256.f), 0, 255);
        if (m_histDragFrom < 0) m_histDragFrom = bin;
        const int a = (std::min)(m_histDragFrom, bin), b = (std::max)(m_histDragFrom, bin);
        dl->AddRectFilled(ImVec2(p0.x + a * w / 256.f, p0.y), ImVec2(p0.x + (b + 1) * w / 256.f, p0.y + h),
                          IM_COL32(255, 255, 255, 40));
    }
    else if (m_histDragFrom >= 0)
    {
        // Released: the dragged bins become the Range mapping (channel R's scale).
        const float mx = std::clamp(ImGui::GetIO().MousePos.x - p0.x, 0.f, w);
        const int bin = std::clamp(static_cast<int>(mx / w * 256.f), 0, 255);
        const int a = (std::min)(m_histDragFrom, bin), b = (std::max)(m_histDragFrom, bin);
        const double lo = s->histLo[0], span = s->histHi[0] - s->histLo[0];
        t.view.map      = TextureView::Map::Range;
        t.view.rangeMin = static_cast<float>(lo + span * a / 256.0);
        t.view.rangeMax = static_cast<float>(lo + span * (b + 1) / 256.0);
        m_histDragFrom  = -1;
    }
}

void TextureViewerWindow::DrawInfo(const TextureViewerContext& ctx, Tab& t)
{
    const Resource::TextureFileInfo& i = t.info;
    auto row = [](const char* label, const char* fmt, auto... args)
    {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(96.f);
        ImGui::Text(fmt, args...);
    };
    // Collapsible sections, the ones read most first: what the texels are
    // (format, size, statistics) before where the file lives and who uses it.
    auto section = [](const char* name, bool open)
    {
        return ImGui::CollapsingHeader(name, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    };
    if (section("Format", true))
    {
        row("Format", "%s", i.formatLabel.c_str());
        row("DXGI", "%s", i.formatName.c_str());
        row("Bits", "%u / texel%s", i.bitsPerPixel, i.isCompressed ? "  (4x4 blocks)" : "");
        row("sRGB", "%s", i.isSRGB ? "yes" : "no");
        row("Alpha", "%s", i.alphaMode.c_str());
        row("Preset", "%s", i.preset.empty() ? "(none of the importer's)" : i.preset.c_str());
    }
    if (section("Size", true))
    {
        if (i.isCube)                  row("Type", "Cube x%u", i.cubeCount);
        else if (i.dimension == "3D")  row("Type", "3D, %u deep", i.depth);
        else if (i.arraySize > 1)      row("Type", "2D array x%u", i.arraySize);
        else                           row("Type", "%s", i.dimension.c_str());
        row("Size", "%u x %u", i.width, i.height);
        row("Mips", "%u", i.mipLevels);
        row("Data", "%s", Resource::FormatByteSize(i.dataBytes).c_str());
        if (ImGui::TreeNode("Mip levels"))
        {
            if (ImGui::BeginTable("##mips", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Mip");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Bytes");
                ImGui::TableHeadersRow();
                for (std::size_t m = 0; m < i.mips.size(); ++m)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::to_string(m).c_str(), t.view.mip == m, ImGuiSelectableFlags_SpanAllColumns))
                        t.view.mip = static_cast<std::uint32_t>(m);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%ux%u", i.mips[m].width, i.mips[m].height);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(
                        Resource::FormatByteSize(i.mips[m].bytes * (i.dimension == "3D" ? 1u : i.arraySize)).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        if (!i.headerMismatch.empty())
        {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s", i.headerMismatch.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    if (section("Statistics", true)) DrawStats(t);
    if (section("GPU", true) && ctx.textures && ctx.resources)
    {
        const Resource::TextureSystem::Status st = ctx.textures->GetStatus(t.handle, *ctx.resources);
        using S = Resource::TextureSystem::Status::State;
        row("State", "%s", st.state == S::Ready ? "resident" : st.state == S::Failed ? "failed" :
                           st.state == S::Loading ? "loading" : "-");
        if (!st.reason.empty())
        {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", st.reason.c_str());
            ImGui::PopTextWrapPos();
        }
        Resource::TextureSystem::Info ti;
        if (ctx.textures->Describe(t.handle, ti) && ti.gpuReady)
        {
            row("VRAM", "%s", Resource::FormatByteSize(ti.gpuBytes).c_str());
            row("Refs", "%u", ti.refCount);
        }
    }
    // How the file was made, and making it again. Import Settings...: unfolded and
    // scrolled to the top of the panel - for a few frames, since the panel's height
    // is only known once it has laid out.
    const bool focusImport = !m_focusImport.empty() && m_focusImport == t.path;
    if (focusImport) ImGui::SetNextItemOpen(true);
    if (section("Import", false))
    {
        if (focusImport)
        {
            ImGui::SetScrollHereY(0.f);
            if (++m_focusImportFrames >= 3)
            {
                m_focusImport.clear();
                m_focusImportFrames = 0;
            }
        }
        DrawImport(ctx, t);
    }
    if (section("File", false))
    {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(t.path.c_str());
        ImGui::PopTextWrapPos();
        if (!i.guid.empty()) row("GUID", "%s", i.guid.c_str());
        row("File", "%s", Resource::FormatByteSize(i.fileBytes).c_str());
        if (ctx.reveal && ImGui::SmallButton("Show in Content Browser")) ctx.reveal(t.path);
        row("Source", "%s", t.sourceFile.empty() ? "(none beside it)" : t.sourceFile.c_str());
    }
    if (section("Used by", false))
    {
        row("Entities", "%u", t.entities);
        if (ImGui::TreeNode("##files", "%zu file%s", t.files.size(), t.files.size() == 1 ? "" : "s"))
        {
            for (const std::string& f : t.files)
                if (ImGui::Selectable(f.c_str()) && ctx.reveal) ctx.reveal(f);
            ImGui::TreePop();
        }
        if (ImGui::SmallButton("Refresh users")) t.usersKnown = false;   // re-read on the next game-half frame
    }
}

void TextureViewerWindow::DrawImport(const TextureViewerContext& ctx, Tab& t)
{
    if (!t.importLoaded)
    {
        ImGui::TextDisabled("(reading its record)");
        return;
    }
    const Resource::TextureImportRecord& rec = t.importRecord;
    Resource::TextureImportSettings& e = t.importEdit;
    if (rec.recorded)
        ImGui::TextDisabled("As its last import recorded them");
    else
    {
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "Guessed");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Imported before settings were recorded: the format is the one its\n"
                              "DXGI format names, the source a same-named image beside it.\n"
                              "A Reimport records what it used.");
    }
    const float labelW = 76.f;
    auto label = [labelW](const char* s)
    {
        ImGui::TextDisabled("%s", s);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-1.f);
    };

    // The source image (editable: Reimport From... in the Content Browser picks one).
    label("Source");
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", t.importSource.c_str());
    if (ImGui::InputTextWithHint("##impsrc", "an image: png jpg tga bmp hdr", buf, sizeof(buf)))
    {
        t.importSource = buf;
        std::error_code ec;
        t.importSourceThere = !t.importSource.empty() &&
                              std::filesystem::is_regular_file(Sys::U8Path(t.importSource), ec);
    }
    if (!t.importSourceThere)
        ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s",
                           t.importSource.empty() ? "No source known - name one to reimport."
                                                  : "Not there - name another to reimport.");

    label("Format");
    if (ImGui::BeginCombo("##impfmt", Resource::TextureImporter::GetFormatName(e.format)))
    {
        for (uint8_t f = 0; f < static_cast<uint8_t>(Resource::TextureImportFormat::Count); ++f)
        {
            const auto fmt = static_cast<Resource::TextureImportFormat>(f);
            if (ImGui::Selectable(Resource::TextureImporter::GetFormatName(fmt), fmt == e.format)) e.format = fmt;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Resource::TextureImporter::GetFormatDescription(fmt));
        }
        ImGui::EndCombo();
    }

    label("Max size");
    static const std::uint32_t kSizes[] = { 0, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32 };
    char sizeLabel[32];
    if (e.maxSize) std::snprintf(sizeLabel, sizeof(sizeLabel), "%u", e.maxSize);
    else           std::snprintf(sizeLabel, sizeof(sizeLabel), "the source's");
    if (ImGui::BeginCombo("##impmax", sizeLabel))
    {
        for (std::uint32_t s : kSizes)
        {
            char l[32];
            if (s) std::snprintf(l, sizeof(l), "%u", s);
            else   std::snprintf(l, sizeof(l), "the source's");
            if (ImGui::Selectable(l, s == e.maxSize)) e.maxSize = s;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The longest edge after import (the aspect is kept)");

    label("Mip filter");
    if (ImGui::BeginCombo("##impmip", Resource::TextureImporter::GetMipFilterId(e.mipFilter)))
    {
        for (uint8_t f = 0; f < static_cast<uint8_t>(Resource::TextureMipFilter::Count); ++f)
        {
            const auto mf = static_cast<Resource::TextureMipFilter>(f);
            if (ImGui::Selectable(Resource::TextureImporter::GetMipFilterId(mf), mf == e.mipFilter)) e.mipFilter = mf;
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Flip green", &e.flipGreen);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("G = 1 - G: a normal map made for the other Y convention");
    ImGui::SameLine();
    ImGui::Checkbox("Mips", &e.generateMips);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build the mip chain (off: one mip)");

    const bool changed = e.format != rec.settings.format || e.maxSize != rec.settings.maxSize ||
                         e.mipFilter != rec.settings.mipFilter || e.flipGreen != rec.settings.flipGreen ||
                         e.generateMips != rec.settings.generateMips || t.importSource != rec.source;
    ImGui::BeginDisabled(!t.importSourceThere || !ctx.reimport);
    if (ImGui::Button(changed ? "Reimport with these" : "Reimport"))
        m_pendingReimports.push_back({ t.path, t.importSource, e });
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Import it again from its source: the same file and guid, swapped in\n"
                          "place wherever it is drawn; the settings are recorded in its .meta.");
    if (changed)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert"))
        {
            t.importEdit   = rec.settings;
            t.importSource = rec.source;
            std::error_code ec;
            t.importSourceThere = !t.importSource.empty() &&
                                  std::filesystem::is_regular_file(Sys::U8Path(t.importSource), ec);
        }
    }
    if (!t.importStatus.empty())
    {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(t.importFailed ? ImVec4(1.f, 0.45f, 0.4f, 1.f) : ImVec4(0.5f, 0.85f, 0.5f, 1.f), "%s",
                           t.importStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

void TextureViewerWindow::Draw(const TextureViewerContext& ctx)
{
    if (!m_open) return;
    ImGui::SetNextWindowSize(ImVec2(1100.f, 720.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Texture Viewer###TextureViewer", &m_open, ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::End();
        return;
    }
    // A texture dragged from the Content Browser opens in a tab.
    auto acceptDrop = [this]()
    {
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                RequestOpen(std::string(static_cast<const char*>(p->Data)));
            ImGui::EndDragDropTarget();
        }
    };

    if (m_tabs.empty())
    {
        ImGui::TextDisabled("Double-click a texture in the Content Browser, or drag one here.");
        ImGui::Dummy(ImGui::GetContentRegionAvail());
        acceptDrop();
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##tvtabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            bool keep = true;
            const std::string label = m_tabs[i].name + "###tvtab" + m_tabs[i].path;
            const ImGuiTabItemFlags f = (i == m_focusTab) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), &keep, f))
            {
                if (m_focusTab < 0 || m_focusTab == i) m_active = i;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", m_tabs[i].path.c_str(),
                                                          Resource::TextureInfoSummary(m_tabs[i].info).c_str());
            if (!keep) m_pendingClose.push_back(i);
        }
        m_focusTab = -1;
        ImGui::EndTabBar();
    }
    Tab* t = ActiveTabPtr();
    if (!t) { ImGui::End(); return; }

    // Keys while the window is focused: [ ] step the mip, F fits.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
    {
        const std::uint32_t mips = (std::max)(1u, t->info.mipLevels);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket) && t->view.mip > 0) --t->view.mip;
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket) && t->view.mip + 1 < mips) ++t->view.mip;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) { t->view.zoom = 0.f; t->userMoved = false; }
    }

    DrawToolbar(*t);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float  statusH = ImGui::GetFrameHeightWithSpacing();
    const float  canvasW = (std::max)(64.f, avail.x - kInfoWidth - ImGui::GetStyle().ItemSpacing.x);
    const float  canvasH = (std::max)(64.f, avail.y - statusH);
    ImGui::BeginChild("##tvcanvaschild", ImVec2(canvasW, canvasH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas(ctx, *t, canvasW, canvasH);
    ImGui::EndChild();
    acceptDrop();
    ImGui::SameLine();
    ImGui::BeginChild("##tvinfo", ImVec2(0.f, canvasH), ImGuiChildFlags_Borders);
    DrawInfo(ctx, *t);
    ImGui::EndChild();

    // Status line: the texel under the cursor.
    if (m_hoverX >= 0)
    {
        // Which picture the texel is from while comparing.
        std::uint32_t bMip = 0;
        const bool comparing = CompareTab(*t, bMip) != nullptr;
        const std::uint32_t mip = m_hoverSideB ? bMip : t->view.mip;
        if (comparing)
        {
            ImGui::TextColored(m_hoverSideB ? ImVec4(1.f, 0.86f, 0.35f, 1.f) : ImVec4(0.6f, 0.8f, 1.f, 1.f), "%s",
                               m_hoverSideB ? "B" : "A");
            ImGui::SameLine();
        }
        if (m_hoverFace >= 0)
            ImGui::Text("(%d, %d) @mip %u %s", m_hoverX, m_hoverY, mip,
                        TextureView::FaceName(static_cast<std::uint32_t>(m_hoverFace)));
        else
            ImGui::Text("(%d, %d) @mip %u", m_hoverX, m_hoverY, mip);
        // What the shaders read there (the GPU probe) and what the file stores (the CPU
        // decode, with the raw bytes / the BC block).
        if (m_probe.gpuValid && m_probe.x == static_cast<std::uint32_t>(m_hoverX) &&
            m_probe.y == static_cast<std::uint32_t>(m_hoverY) && m_probe.sideB == m_hoverSideB)
        {
            ImGui::SameLine(0.f, 16.f);
            ImGui::Text("value %.4f %.4f %.4f %.4f", m_probe.gpu[0], m_probe.gpu[1], m_probe.gpu[2], m_probe.gpu[3]);
            if (m_probe.stored.ok)
            {
                ImGui::SameLine(0.f, 16.f);
                ImGui::TextDisabled("stored %.4f %.4f %.4f %.4f  %s %s", m_probe.stored.value[0], m_probe.stored.value[1],
                                    m_probe.stored.value[2], m_probe.stored.value[3],
                                    m_probe.stored.block ? "block" : "raw", m_probe.stored.raw.c_str());
            }
        }
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextDisabled("uv (%.4f, %.4f)", m_hoverU, m_hoverV);
    }
    else
        ImGui::TextDisabled("%s", Resource::TextureInfoSummary(t->info).c_str());
    ImGui::End();
}

// ==== Harness ====
bool TextureViewerWindow::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    static const char* kFlags[] = {
        "-texviewer_size", "-texviewer_view", "-texviewer_shot", "-texviewer_close_at",
        "-texviewer_channel_off", "-texviewer_mip_off", "-texviewer_face_off", "-texviewer_map_off",
        "-texviewer_trust_header", "-texviewer_redraw_every_frame", "-texviewer_probe", "-texviewer_stats",
        "-texviewer_compare_off",
    };
    const bool hasOpen = cl.Has("-texviewer_open");
    for (const char* f : kFlags)
        if (cl.Has(f) && !hasOpen)
        {
            why = std::string(f) + " without -texviewer_open=<path.itex> has no subject";
            return false;
        }
    if (!hasOpen) return true;
    // A comma list (a comparison's picture B is another tab); the first is active.
    m_harnessOpen = cl.Get("-texviewer_open");
    for (const std::string& item : SplitList(m_harnessOpen, ','))
        m_harnessOpenList.push_back(Resource::AssetRefs::PercentDecode(item));
    if (m_harnessOpenList.empty())
    {
        why = "-texviewer_open needs a .itex path (or a comma list of them)";
        return false;
    }
    if (static_cast<int>(m_harnessOpenList.size()) > kMaxTabs)
    {
        why = "-texviewer_open names more than " + std::to_string(kMaxTabs) + " textures";
        return false;
    }
    m_harnessChannelOff = cl.Has("-texviewer_channel_off");
    m_harnessMipOff     = cl.Has("-texviewer_mip_off");
    m_harnessFaceOff    = cl.Has("-texviewer_face_off");
    m_harnessMapOff     = cl.Has("-texviewer_map_off");
    m_harnessTrustHeader = cl.Has("-texviewer_trust_header");
    m_harnessRedrawAll   = cl.Has("-texviewer_redraw_every_frame");
    m_harnessCompareOff  = cl.Has("-texviewer_compare_off");
    if (cl.Has("-texviewer_size"))
    {
        unsigned w = 0, h = 0;
        if (sscanf_s(cl.Get("-texviewer_size").c_str(), "%ux%u", &w, &h) != 2 || w < 16 || h < 16 || w > 4096 || h > 4096)
        {
            why = "-texviewer_size needs <w>x<h> (16..4096)";
            return false;
        }
        PinSize(w, h);
    }
    auto list = [&](const char* flag, int kind, bool path) -> bool
    {
        if (!cl.Has(flag)) return true;
        const std::string v = cl.Get(flag);
        if (v.empty()) { why = std::string(flag) + " needs a value"; return false; }
        for (const std::string& item : SplitList(v, ','))
        {
            Step s;
            if (!SplitAt(item, s.text, s.at)) { why = std::string(flag) + ": '" + item + "' is not <value>@<frame>"; return false; }
            s.kind = kind;
            if (path) s.text = HarnessPath(s.text);
            m_steps.push_back(std::move(s));
        }
        return true;
    };
    if (!list("-texviewer_view", kStepView, false) || !list("-texviewer_shot", kStepShot, true) ||
        !list("-texviewer_probe", kStepProbe, false) || !list("-texviewer_stats", kStepStats, false))
        return false;
    // probe = <x>:<y>[:<mip>[:<face>]], stats = <mip>[:<face>] - numbers only.
    for (const Step& s : m_steps)
        if (s.kind == kStepProbe || s.kind == kStepStats)
        {
            const std::vector<std::string> parts = SplitList(s.text, ':');
            const std::size_t lo = s.kind == kStepProbe ? 2 : 1, hi = s.kind == kStepProbe ? 4 : 2;
            std::uint32_t tmp = 0;
            bool good = parts.size() >= lo && parts.size() <= hi;
            for (const std::string& p : parts) good = good && ParseUInt(p, tmp);
            if (!good)
            {
                why = s.kind == kStepProbe ? "-texviewer_probe: '" + s.text + "' is not <x>:<y>[:<mip>[:<face>]]"
                                           : "-texviewer_stats: '" + s.text + "' is not <mip>[:<face>]";
                return false;
            }
        }
    // A view spec is checked now against the grammar (its values need the
    // subject, so they are checked again when the step runs).
    for (const Step& s : m_steps)
        if (s.kind == kStepView)
            for (const std::string& kv : SplitList(s.text, ';'))
                if (kv.find('=') == std::string::npos || kv.front() == '=')
                {
                    why = "-texviewer_view: '" + kv + "' is not key=value (keys ch, mip, face, layout, map, ev, tonemap, "
                          "range, bg, filtered, tile, zoom, pan, size, cmp, with, split, k; ';' between them)";
                    return false;
                }
    if (cl.Has("-texviewer_close_at"))
    {
        std::uint32_t f = 0;
        if (!ParseUInt(cl.Get("-texviewer_close_at"), f)) { why = "-texviewer_close_at needs an editor frame"; return false; }
        Step s;
        s.kind = kStepClose;
        s.at   = f;
        m_steps.push_back(std::move(s));
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    LOG_INFO("[TEXVIEWER] harness: open='%s' steps=%zu size=%ux%u channelOff=%d mipOff=%d faceOff=%d mapOff=%d "
             "trustHeader=%d redrawAll=%d compareOff=%d", m_harnessOpen.c_str(), m_steps.size(), m_pinW, m_pinH,
             m_harnessChannelOff ? 1 : 0, m_harnessMipOff ? 1 : 0, m_harnessFaceOff ? 1 : 0, m_harnessMapOff ? 1 : 0,
             m_harnessTrustHeader ? 1 : 0, m_harnessRedrawAll ? 1 : 0, m_harnessCompareOff ? 1 : 0);
    return true;
}

void TextureViewerWindow::TickHarness(const TextureViewerContext& ctx)
{
    if (m_harnessOpen.empty() || m_harnessFailed) return;
    if (!m_harnessOpened)
    {
        m_harnessOpened = true;
        for (const std::string& path : m_harnessOpenList)
        {
            std::string why;
            if (!OpenNow(ctx, path, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-texviewer_open='%s' cannot open the Texture Viewer: %s", path.c_str(), why.c_str());
                return;
            }
        }
        Activate(0);
    }
    for (Step& s : m_steps)
    {
        if (s.done || ctx.editorFrame < s.at) continue;
        switch (s.kind)
        {
        case kStepView:
        {
            std::string why;
            if (!ApplySpec(s.text, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-texviewer_view '%s' at editor frame %llu: %s", s.text.c_str(),
                          static_cast<unsigned long long>(s.at), why.c_str());
            }
            else
                LOG_INFO("[TEXVIEWER] harness view -> %s (frame %llu)", s.text.c_str(),
                         static_cast<unsigned long long>(ctx.editorFrame));
            s.done = true;
            // The new description reaches the view in this frame's ProcessPendingActions
            // below: a shot on the same frame waits for its render.
            return;
        }
        case kStepShot:
        {
            if (!Settled())
            {
                if (ctx.editorFrame > s.at + kShotDeadline)
                {
                    s.done = true;
                    m_harnessFailed = true;
                    LOG_ERROR("[TEXVIEWER] shot '%s' at editor frame %llu: the picture never settled (renders=%llu)",
                              s.text.c_str(), static_cast<unsigned long long>(s.at),
                              static_cast<unsigned long long>(m_lastRenders));
                }
                return;   // steps run in order
            }
            std::string why;
            if (!WriteShot(ctx, s.text, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-texviewer_shot '%s' failed: %s", s.text.c_str(), why.c_str());
            }
            s.done = true;
            break;
        }
        case kStepClose:
            CloseAll(ctx);
            m_open = false;
            s.done = true;
            break;
        case kStepProbe:
        {
            // x:y[:mip[:face]] - the mip / face become the view's, then the probe
            // is queued (it answers kFramesInFlight frames after it is armed).
            const std::vector<std::string> p = SplitList(s.text, ':');
            std::uint32_t x = 0, y = 0;
            ParseUInt(p[0], x);
            ParseUInt(p[1], y);
            std::string why;
            if (p.size() > 2 && !Set("mip", p[2], why)) { m_harnessFailed = true; LOG_ERROR("-texviewer_probe: %s", why.c_str()); }
            int face = -1;
            if (p.size() > 3)
            {
                std::uint32_t f = 0;
                ParseUInt(p[3], f);
                face = static_cast<int>(f);
            }
            PendingProbe q;
            q.x = x; q.y = y; q.face = face; q.harness = true; q.queuedAt = ctx.editorFrame;
            m_probes.push_back(q);
            s.done = true;
            break;
        }
        case kStepStats:
        {
            const std::vector<std::string> p = SplitList(s.text, ':');
            Resource::AnalyzeOptions o;
            ParseUInt(p[0], o.mip);
            if (p.size() > 1) ParseUInt(p[1], o.slice);
            o.normalCheck = true;
            o.mipChain    = true;
            m_statsQueue.push_back({ o, 0 });   // job 0 = the harness: logs its line
            s.done = true;
            break;
        }
        default:
            s.done = true;
            break;
        }
    }
}

} // namespace Editor
