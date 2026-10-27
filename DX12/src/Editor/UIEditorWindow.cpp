// UIEditorWindow.cpp - the UI Editor window; its contract is in UIEditorWindow.h.
#include "Editor/UIEditorWindow.h"

#include "ECS/GuidComponent.h"
#include "ECS/HierarchyComponents.h"         // SceneTransientTag, PreviewStageTag, PrefabSourcePath
#include "ECS/HierarchyOps.h"                // a stage goes with Hierarchy::DestroySubtree
#include "Editor/EditorLayerShared.h"        // SaveFileDialog (Save As, Convert, Save Branch)
#include "Editor/EditorTheme.h"             // Icon()
#include "Editor/FontAwesomeIcons.h"
#include "Editor/UITemplates.h"             // the Palette's widgets (shared with Create > UI)
#include "Editor/Undo/ComponentBlob.h"
#include "Editor/Undo/ComponentEditAction.h"
#include "Editor/Undo/EditSink.h"            // an asset tab's own ring
#include "Editor/Undo/EntitySnapshot.h"      // a stage detached for Play and attached again
#include "ECS/GuidRegistry.h"
#include "Resource/AssetGuid.h"              // a saved .iui's identity (.meta)
#include "Graphics/IGraphicsDevice.h"
#include "Harness/ControlChannelJobs.h"
#include "Renderer/Renderer.h"
#include "Renderer/UIViewService.h"
#include "Resource/AssetFS.h"                // the Check page: where a font file is
#include "Resource/AssetRefs.h"
#include "Resource/ComponentSerializers.h"   // GetComponentRegistry
#include "Resource/TextureInfo.h"            // a dropped texture's size (Native Size)
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "UI/UIAsset.h"              // .iui templates in the Palette and drops
#include "UI/UICanvas.h"
#include "UI/UIDocument.h"           // UI Document instances are read-only here; Extract

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ostream>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>

namespace Editor
{
namespace
{
    constexpr std::uint64_t kSettleFrames  = 4;     // a complete picture this long = final
    constexpr std::uint64_t kLoadGrace     = 120;   // frames an image may stay unloaded before a shot takes it as is
    constexpr std::uint64_t kShotDeadline  = 400;   // editor frames a shot may wait
    constexpr std::uint64_t kOpenDeadline  = 600;   // editor frames -uieditor_open waits for its canvas
    constexpr float         kMinZoom       = 1.f / 64.f;
    constexpr float         kMaxZoom       = 32.f;
    constexpr float         kFitMargin     = 16.f;  // view px around a fitted frame
    constexpr std::uint32_t kDefaultBoxW   = 1280;  // the view box with no canvas (a harness run)
    constexpr std::uint32_t kDefaultBoxH   = 720;
    constexpr float         kMinDesign     = 16.f;
    constexpr float         kMaxDesign     = 8192.f;
    constexpr std::uint32_t kWorkspaceRgba = 0xFF1A1A1Au;   // outside the design frame (Color32 packing)
    constexpr float         kSnapViewPx    = 6.f;   // snap distance, view px
    constexpr float         kHandleViewPx  = 4.5f;  // a handle's half size, view px (drawn)
    constexpr float         kGrabViewPx    = 8.f;   // a handle's grab radius, view px
    constexpr std::uint64_t kDiskPollFrames = 30;   // an asset tab looks at its file (and captures) this often
    constexpr const char*   kIuiFilter     = "UI Asset (*.iui)\0*.iui\0";
    struct AnchorPreset { const char* name; float minX, minY, maxX, maxY, pivX, pivY; };
    // The 4 x 4 grid UMG / uGUI show: rows top / middle / bottom / stretch, columns
    // left / center / right / stretch. Alt also takes the pivot, Shift the position.
    constexpr AnchorPreset kAnchorPresets[16] = {
        { "top_left",       0.f, 0.f,  0.f, 0.f,  0.f, 0.f  }, { "top_center",     .5f, 0.f, .5f, 0.f,  .5f, 0.f  },
        { "top_right",      1.f, 0.f,  1.f, 0.f,  1.f, 0.f  }, { "top_stretch",    0.f, 0.f, 1.f, 0.f,  .5f, 0.f  },
        { "middle_left",    0.f, .5f,  0.f, .5f,  0.f, .5f  }, { "middle_center",  .5f, .5f, .5f, .5f,  .5f, .5f  },
        { "middle_right",   1.f, .5f,  1.f, .5f,  1.f, .5f  }, { "middle_stretch", 0.f, .5f, 1.f, .5f,  .5f, .5f  },
        { "bottom_left",    0.f, 1.f,  0.f, 1.f,  0.f, 1.f  }, { "bottom_center",  .5f, 1.f, .5f, 1.f,  .5f, 1.f  },
        { "bottom_right",   1.f, 1.f,  1.f, 1.f,  1.f, 1.f  }, { "bottom_stretch", 0.f, 1.f, 1.f, 1.f,  .5f, 1.f  },
        { "stretch_left",   0.f, 0.f,  0.f, 1.f,  0.f, .5f  }, { "stretch_center", .5f, 0.f, .5f, 1.f,  .5f, .5f  },
        { "stretch_right",  1.f, 0.f,  1.f, 1.f,  1.f, .5f  }, { "stretch_full",   0.f, 0.f, 1.f, 1.f,  .5f, .5f  },
    };

    enum StepKind { kStepView = 1, kStepShot, kStepClose, kStepSelect, kStepDrag, kStepAnchor };

    // The resolution presets.
    struct Preset { const char* label; std::uint32_t w, h; };
    constexpr Preset kPresets[] = {
        { "1280 x 720",           1280,  720 }, { "1920 x 1080",          1920, 1080 },
        { "2560 x 1440",          2560, 1440 }, { "3840 x 2160",          3840, 2160 },
        { "1920 x 1200 (16:10)",  1920, 1200 }, { "2560 x 1080 (21:9)",   2560, 1080 },
        { "3440 x 1440 (21:9)",   3440, 1440 }, { "5120 x 1440 (32:9)",   5120, 1440 },
        { "1280 x 800 (Deck)",    1280,  800 }, { "1024 x 768 (4:3)",     1024,  768 },
        { "1080 x 1920 (portrait)", 1080, 1920 },
    };

    // A normalised anchor / pivot value snapped to 0, 0.5 or 1 within @p thr.
    float SnapUnit(float v, float thr)
    {
        for (const float t : { 0.f, 0.5f, 1.f })
            if (std::fabs(v - t) <= thr) return t;
        return v;
    }

    // An image's 9-slice borders as the paint cuts them (EmitNineSlice in UICanvasLayout.cpp):
    // texture px drawn 1:1 in destination px, opposite borders shrunk together when they
    // do not fit. Left, top, right, bottom.
    void EffectiveBorders(const UI::UIImage& img, const UI::Rect& r, float b[4])
    {
        const float w = r.mx.x - r.mn.x, h = r.mx.y - r.mn.y;
        b[0] = (std::max)(0.f, img.sliceBorder.x);
        b[1] = (std::max)(0.f, img.sliceBorder.y);
        b[2] = (std::max)(0.f, img.sliceBorder.z);
        b[3] = (std::max)(0.f, img.sliceBorder.w);
        if (b[0] + b[2] > w && b[0] + b[2] > 0.f) { const float k = (std::max)(0.f, w) / (b[0] + b[2]); b[0] *= k; b[2] *= k; }
        if (b[1] + b[3] > h && b[1] + b[3] > 0.f) { const float k = (std::max)(0.f, h) / (b[1] + b[3]); b[1] *= k; b[3] *= k; }
    }

    // Where border i's drag diamond sits (0 left, 1 top, 2 right, 3 bottom): on its
    // line, a quarter of the way along, clear of the rect's own handles.
    UI::Vec2 SliceHandlePoint(const UI::Rect& r, const float b[4], int i)
    {
        const float w = r.mx.x - r.mn.x, h = r.mx.y - r.mn.y;
        switch (i)
        {
        case 0:  return { r.mn.x + b[0], r.mn.y + h * 0.25f };
        case 1:  return { r.mn.x + w * 0.25f, r.mn.y + b[1] };
        case 2:  return { r.mx.x - b[2], r.mn.y + h * 0.75f };
        default: return { r.mn.x + w * 0.75f, r.mx.y - b[3] };
        }
    }

    bool HasIn(const std::vector<Entity>& v, Entity e) { return std::find(v.begin(), v.end(), e) != v.end(); }
    // Put @p e in or out of @p v; true when that changed it.
    bool SetIn(std::vector<Entity>& v, Entity e, bool on)
    {
        const auto it = std::find(v.begin(), v.end(), e);
        if (on == (it != v.end())) return false;
        if (on) v.push_back(e);
        else v.erase(it);
        return true;
    }

    // ImGui::InputTextMultiline over a growing std::vector<char> (no imgui_stdlib here).
    int TextBufResize(ImGuiInputTextCallbackData* d)
    {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto* buf = static_cast<std::vector<char>*>(d->UserData);
            buf->resize(static_cast<std::size_t>(d->BufTextLen) + 1);
            d->Buf = buf->data();
        }
        return 0;
    }

    // A UI template: an .ipfb whose ROOT node (the first "N idx=0" block of its
    // text, after a binary header - so a byte search, not a line read) has a
    // CvRect or a CvCanvas.
    bool IsUIPrefabFile(const std::filesystem::path& p)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        if (ec || size == 0 || size > (16u << 20)) return false;
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return false;
        std::string s(static_cast<std::size_t>(size), '\0');
        const std::size_t got = std::fread(s.data(), 1, s.size(), f);
        std::fclose(f);
        s.resize(got);
        const std::size_t root = s.find("\nN idx=0 ");
        if (root == std::string::npos) return false;
        std::size_t end = s.find("\nN ", root + 1);
        if (end == std::string::npos) end = s.size();
        const std::string block = s.substr(root, end - root);
        return block.find("\n  CvRect:") != std::string::npos || block.find("\n  CvCanvas:") != std::string::npos;
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
        if (s.empty() || s.size() > 9 ||
            !std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
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

    // "WxH" (also "W:H") -> two whole numbers.
    bool ParseSize(const std::string& s, std::uint32_t& w, std::uint32_t& h)
    {
        std::size_t p = s.find('x');
        if (p == std::string::npos) p = s.find('X');
        if (p == std::string::npos) p = s.find(':');
        return p != std::string::npos && ParseUInt(s.substr(0, p), w) && ParseUInt(s.substr(p + 1), h);
    }

    bool ParseState(const std::string& s, UI::StatePreview& out)
    {
        const std::string l = Lower(s);
        if (l == "live")     { out = UI::StatePreview::Live;     return true; }
        if (l == "normal")   { out = UI::StatePreview::Normal;   return true; }
        if (l == "hovered")  { out = UI::StatePreview::Hovered;  return true; }
        if (l == "pressed")  { out = UI::StatePreview::Pressed;  return true; }
        if (l == "disabled") { out = UI::StatePreview::Disabled; return true; }
        return false;
    }

    std::uint64_t Mix(std::uint64_t h, std::uint64_t v)
    {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
    std::uint64_t Bits(float f)
    {
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    // Everything of a description that changes the picture (not its serial).
    std::uint64_t HashDesc(const UIView::Desc& d)
    {
        std::uint64_t h = 0x84222325cbf29ce4ull;
        h = Mix(h, d.subject);
        h = Mix(h, d.worldEpoch);
        h = Mix(h, Bits(d.designSize.x));
        h = Mix(h, Bits(d.designSize.y));
        h = Mix(h, static_cast<std::uint64_t>(d.frame.mode));
        h = Mix(h, Bits(d.frame.size.x));
        h = Mix(h, Bits(d.frame.size.y));
        h = Mix(h, d.hasScaler ? 1u : 0u);
        h = Mix(h, static_cast<std::uint64_t>(d.scaler.scaleMode));
        h = Mix(h, Bits(d.scaler.referenceResolution.x));
        h = Mix(h, Bits(d.scaler.referenceResolution.y));
        h = Mix(h, Bits(d.scaler.matchWidthOrHeight));
        h = Mix(h, d.targetW);
        h = Mix(h, d.targetH);
        h = Mix(h, Bits(d.zoom));
        h = Mix(h, Bits(d.pan.x));
        h = Mix(h, Bits(d.pan.y));
        h = Mix(h, static_cast<std::uint64_t>(d.background));
        h = Mix(h, d.solidRgba);
        h = Mix(h, d.workspaceRgba);
        h = Mix(h, d.clipToFrame ? 1u : 0u);
        h = Mix(h, static_cast<std::uint64_t>(d.state));
        h = Mix(h, d.stateTarget);
        h = Mix(h, d.showHidden ? 1u : 0u);
        h = Mix(h, d.realtime ? 1u : 0u);
        h = Mix(h, d.debug);
        h = Mix(h, d.skip.size());
        for (const Entity e : d.skip) h = Mix(h, e);
        h = Mix(h, d.freeze ? 1u : 0u);
        return h;
    }

    std::string HexGuid(const ECS::Guid& g) { return g.IsValid() ? g.ToHex() : std::string(); }

    // ---- An asset tab's file ----
    // The form a tab keys its file by - root-relative, forward slashes: what
    // Engine.SpawnUI derives node GUIDs from and the Content Browser hands over.
    std::string CanonAsset(const std::string& p)
    {
        std::string s = Sys::RelativizeAssetPath(p);
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }

    bool EndsWithCI(const std::string& s, const char* ext)
    {
        const std::size_t n = std::strlen(ext);
        if (s.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
                std::tolower(static_cast<unsigned char>(ext[i])))
                return false;
        return true;
    }

    // Where the loaders read @p path on disk (this checkout, then a worktree's
    // shared content); "" when it is not on disk.
    std::string OnDisk(const std::string& path)
    {
        std::string p = Resource::AssetFS::Get().ResolveOnDisk(path);
        if (p.empty())
        {
            std::error_code ec;
            if (std::filesystem::exists(Sys::U8Path(path), ec)) p = path;
        }
        return p;
    }

    bool StatFile(const std::string& path, std::int64_t& stamp, std::uint64_t& size)
    {
        const std::string p = OnDisk(path);
        if (p.empty()) return false;
        std::error_code ec;
        const auto u = Sys::U8Path(p);
        size = static_cast<std::uint64_t>(std::filesystem::file_size(u, ec));
        if (ec) return false;
        const auto t = std::filesystem::last_write_time(u, ec);
        if (ec) return false;
        stamp = static_cast<std::int64_t>(t.time_since_epoch().count());
        return true;
    }

    // Where a save of @p path may write: this checkout's file, or a new one at @p path
    // - never the main checkout's shared content behind a worktree (AssetFS's shared
    // root is read-only) and never a packaged entry. False and @p why for those.
    bool WritableTarget(const std::string& path, std::string& target, std::string& why)
    {
        const Resource::AssetFS& fs = Resource::AssetFS::Get();
        target = fs.ResolveLocal(path);
        if (!target.empty()) return true;
        const std::string onDisk = fs.ResolveOnDisk(path);
        if (!onDisk.empty() && fs.IsSharedPath(onDisk))
        {
            why = "'" + path + "' is read from another checkout's shared content (this is a worktree): saving it "
                  "would change that checkout - Save As a file of this one";
            return false;
        }
        if (onDisk.empty() && fs.HasInPak(path))
        {
            why = "'" + path + "' is packaged content";
            return false;
        }
        target = onDisk.empty() ? path : onDisk;
        return true;
    }

    std::string FrameText(const UIView::RootFrame& f)
    {
        if (f.mode == UIView::RootFrame::Mode::Fill) return "fill";
        char b[48];
        std::snprintf(b, sizeof(b), "%gx%g", f.size.x, f.size.y);
        return b;
    }
}

UIEditorWindow& UIEditorWindow::Get()
{
    static UIEditorWindow s_instance;
    return s_instance;
}

const char* UIEditorWindow::ResolutionName(Resolution r)
{
    switch (r)
    {
    case Resolution::MatchMain: return "match_main";
    case Resolution::Custom:    return "custom";
    default:                    return "reference";
    }
}

const char* UIEditorWindow::BackgroundName(UIView::Background b)
{
    switch (b)
    {
    case UIView::Background::Solid: return "solid";
    case UIView::Background::None:  return "none";
    default:                        return "checker";
    }
}

const char* UIEditorWindow::StateName(UI::StatePreview s)
{
    switch (s)
    {
    case UI::StatePreview::Normal:   return "normal";
    case UI::StatePreview::Hovered:  return "hovered";
    case UI::StatePreview::Pressed:  return "pressed";
    case UI::StatePreview::Disabled: return "disabled";
    default:                         return "live";
    }
}

UIEditorWindow::Tab* UIEditorWindow::ActiveTabPtr()
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? &m_tabs[m_active] : nullptr;
}

const UIEditorWindow::Tab* UIEditorWindow::ActiveTabPtr() const
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? &m_tabs[m_active] : nullptr;
}

// ==== Tabs ====
void UIEditorWindow::RequestOpen(Entity e)
{
    m_pendingOpen.push_back(e);
    m_open = true;
}

Entity UIEditorWindow::FindCanvasByName(World& world, const std::string& name)
{
    auto* pool = world.GetPool<UI::UICanvas>();
    if (!pool) return NullEntity;
    for (const Entity e : pool->Entities())
        if (world.GetName(e) == name) return e;
    return NullEntity;
}

bool UIEditorWindow::OpenNow(const UIEditorContext& ctx, Entity e, std::string& why, int* tabOut)
{
    if (!ctx.world) { why = "the editor has no world"; return false; }
    World& w = *ctx.world;
    if (e == NullEntity || !w.IsAlive(e)) { why = "the entity is not alive"; return false; }
    // Never under Draw's Tab* (a push_back may move every tab): the PPA opens it.
    if (m_inDraw)
    {
        RequestOpen(e);
        if (tabOut) *tabOut = -1;
        return true;
    }

    // Up the UI tree to its root: the canvas, or the topmost UI node under none
    // (a UI subtree no canvas owns lays out against the design screen).
    Entity root = NullEntity;
    Entity cur  = e;
    for (int guard = 0; guard < 4096 && cur != NullEntity; ++guard)
    {
        if (w.HasComponent<UI::UICanvas>(cur)) { root = cur; break; }
        if (!w.HasComponent<UI::UIRect>(cur)) break;
        root = cur;
        const UI::UIParent* p = w.GetComponent<UI::UIParent>(cur);
        Entity next = p ? p->parent : NullEntity;
        if (next == NullEntity && p && p->parentGuid.IsValid()) next = ECS::GuidRegistry::Get().Find(p->parentGuid);
        if (next == NullEntity || next == cur || !w.IsAlive(next)) break;
        cur = next;
    }
    if (root == NullEntity)
    {
        why = "'" + w.GetName(e) + "' is not a UI node (it has neither a UICanvas nor a UIRect)";
        return false;
    }
    // A node of an asset / template tab's stage is that tab's (never a scene tab:
    // its edits would land in the scene's history).
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (m_tabs[i].kind != TabKind::Scene && m_tabs[i].subject == root)
        {
            Activate(i);
            m_open        = true;
            m_focusWindow = true;
            if (e != root) m_tabs[i].sel = e;
            if (tabOut) *tabOut = i;
            return true;
        }
    if (const PreviewStageTag* st = w.GetComponent<PreviewStageTag>(root))
    {
        why = st->owner == PreviewStageTag::kUIEditor ? "a UI Editor stage node whose tab is closed"
                                                       : "a preview stage of another editor panel";
        return false;
    }

    int tab = -1;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (m_tabs[i].subject == root && m_tabs[i].epoch == w.Epoch()) { tab = i; break; }
    if (tab < 0)
    {
        if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
        {
            why = "already " + std::to_string(kMaxTabs) + " tabs open - close one";
            return false;
        }
        Tab t;
        t.uid     = m_nextUid++;
        t.subject = root;
        t.epoch   = w.Epoch();
        if (const GuidComponent* gc = w.GetComponent<GuidComponent>(root)) t.guid = gc->guid;
        t.name = w.GetName(root);
        ApplyResPref(t);   // the resolution this canvas was last edited at
        DesignSize(ctx, t, t.designW, t.designH);
        m_tabs.push_back(std::move(t));
        tab = static_cast<int>(m_tabs.size()) - 1;
        const Tab& n = m_tabs.back();
        LOG_INFO("[UIEDITOR] open tab='%s' subject=%u kind=%s guid=%s design=%gx%g res=%s", n.name.c_str(),
                 static_cast<unsigned>(root), w.HasComponent<UI::UICanvas>(root) ? "canvas" : "node",
                 HexGuid(n.guid).c_str(), n.designW, n.designH, ResolutionName(n.view.res));
    }
    m_active      = tab;
    m_focusTab    = tab;
    m_open        = true;
    m_focusWindow = true;
    // Opened from a node: that node is the selection (scene tabs share it).
    if (e != root && ctx.select) ctx.select(e);
    if (tabOut) *tabOut = tab;
    return true;
}

bool UIEditorWindow::CloseTab(const UIEditorContext& ctx, int tab, bool discard, std::string* why)
{
    if (tab < 0) tab = m_active;
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size()))
    {
        if (why) *why = "no such tab";
        return false;
    }
    Tab& t = m_tabs[tab];
    const bool unsaved = t.kind == TabKind::Asset && t.stagedText != t.savedText;
    if (unsaved && !discard)
    {
        if (why) *why = "'" + t.path + "' has unsaved edits: Save it, or close with discard";
        return false;
    }
    // Never under Draw's Tab*: the PPA closes it (by uid).
    if (m_inDraw)
    {
        m_pendingClose.push_back(t.uid);
        return true;
    }
    // A gesture held on this tab ends as ITS step, before its ring goes.
    if (tab == m_active) CommitGestures(ctx);
    if (tab == m_viewTab) ReleaseView(ctx);
    else if (m_viewTab > tab) --m_viewTab;
    // An asset / template tab's stage goes with it. A stage parked for Play has its
    // ids parked in the played world AND in the edit world it comes back to - they
    // are released after Stop.
    if (t.kind != TabKind::Scene && ctx.world) DestroyStage(*ctx.world, t);
    if (t.parkedStage)
    {
        if (ctx.playing) m_parkedDiscards.push_back(t.parkedStage);
        else if (ctx.world)
            for (const auto& n : t.parkedStage->nodes) ctx.world->UnreserveId(n.id);
        t.parkedStage.reset();
    }
    // And its ring: a Delete step or an undone Create holds parked ids that only
    // its OnDiscard hands back (the edit world's: after Stop when in Play).
    if (t.sink)
    {
        if (ctx.playing) m_parkedSinks.push_back(t.sink);
        else t.sink->history.Clear();
    }
    if (m_confirmClose == t.uid) m_confirmClose = 0;
    LOG_INFO("[UIEDITOR] close tab='%s'%s", t.name.c_str(), unsaved ? " (unsaved edits discarded)" : "");
    m_tabs.erase(m_tabs.begin() + tab);
    if (m_active > tab || m_active >= static_cast<int>(m_tabs.size())) --m_active;
    return true;
}

void UIEditorWindow::CloseAll(const UIEditorContext& ctx)
{
    if (m_inDraw)   // the PPA closes them (CloseTab would only queue each one)
    {
        for (const Tab& t : m_tabs) m_pendingClose.push_back(t.uid);
        return;
    }
    while (!m_tabs.empty()) CloseTab(ctx, static_cast<int>(m_tabs.size()) - 1);
    m_active = -1;
}

void UIEditorWindow::SwitchTab(const UIEditorContext& ctx, int tab)
{
    if (tab == m_active) return;
    // A drag or an arrow-key nudge still held is the OLD tab's step: committed now,
    // while its tab is the active one (the ctx callbacks pick the active tab's ring).
    CommitGestures(ctx);
    m_active = tab;
}

bool UIEditorWindow::Activate(int tab)
{
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size())) return false;
    m_active   = tab;
    m_focusTab = tab;
    return true;
}

void UIEditorWindow::ReleaseView(const UIEditorContext& ctx)
{
    if (m_viewId != ~0u && ctx.renderer)
        if (UIViewService* views = ctx.renderer->GetUIViews()) views->Release(m_viewId);
    m_viewId  = ~0u;
    m_viewTab = -1;
    m_lastDescHash = 0;
    m_hasLastDesc  = false;
    m_shown = m_complete = false;
}

void UIEditorWindow::ReleaseAtShutdown(const UIEditorContext& ctx)
{
    CloseAll(ctx);
    ReleaseView(ctx);
    for (const PendingShot& s : m_shots) Harness::Jobs::Fail(s.job, "closed", "the editor is shutting down", {});
    m_shots.clear();
}

bool UIEditorWindow::Revalidate(const UIEditorContext& ctx, Tab& t, std::string& why)
{
    World& w = *ctx.world;
    auto isRoot = [&w](Entity e) { return w.HasComponent<UI::UICanvas>(e) || w.HasComponent<UI::UIRect>(e); };
    auto sameGuid = [&w, &t](Entity e)
    {
        if (!t.guid.IsValid()) return true;
        const GuidComponent* gc = w.GetComponent<GuidComponent>(e);
        return gc && gc->guid == t.guid;
    };
    if (t.epoch == w.Epoch())
    {
        // Ids are reused within a world: the GUID tells "the same canvas" from
        // "another entity on its old id".
        if (!w.IsAlive(t.subject) || !isRoot(t.subject) || !sameGuid(t.subject))
        {
            why = "the canvas was deleted";
            return false;
        }
        return true;
    }
    // Another world (a scene load, Play / Stop): find the canvas again by its GUID.
    Entity e = NullEntity;
    if (t.guid.IsValid())
    {
        e = ECS::GuidRegistry::Get().Find(t.guid);
        if (e != NullEntity && (!w.IsAlive(e) || !sameGuid(e))) e = NullEntity;
        if (e == NullEntity)
            if (auto* pool = w.GetPool<UI::UICanvas>())
                for (const Entity c : pool->Entities())
                    if (sameGuid(c)) { e = c; break; }
    }
    if (e == NullEntity || !isRoot(e))
    {
        why = t.guid.IsValid() ? "the canvas is not in the new world" : "the world changed and the canvas has no GUID to find it by";
        return false;
    }
    LOG_INFO("[UIEDITOR] tab '%s' follows its canvas into world epoch %u (entity %u -> %u)", t.name.c_str(),
             w.Epoch(), static_cast<unsigned>(t.subject), static_cast<unsigned>(e));
    t.subject = e;
    t.epoch   = w.Epoch();
    return true;
}

// ==== Asset tabs ====
namespace
{
    // The id the selection was captured with (a respawn finds it again by its id).
    std::string IdOf(const std::vector<std::pair<Entity, std::string>>& ids, Entity e)
    {
        for (const auto& [ent, id] : ids)
            if (ent == e) return id;
        return std::string();
    }

    std::string IssueText(const std::string& path, const std::vector<UI::UIAssetIssue>& issues)
    {
        std::string s = path + ":";
        for (std::size_t i = 0; i < issues.size() && i < 4; ++i)
            s += " [" + issues[i].where + "] " + issues[i].message + ";";
        if (issues.size() > 4) s += " ... " + std::to_string(issues.size() - 4) + " more";
        return s;
    }

    // Write @p text (a canonical .iui) to @p path: read back first - a text the parser
    // refuses never lands on disk - then atomically, then its identity (.meta) and the
    // load cache. @p nodes: how many it holds.
    bool WriteIui(const std::string& path, const std::string& text, std::string& why, std::size_t* nodes)
    {
        UI::UIAsset back;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(text, back, issues))
        {
            why = "the captured asset does not read back, nothing written: " + IssueText(path, issues);
            return false;
        }
        std::string target;
        if (!WritableTarget(path, target, why)) return false;
        std::error_code ec;
        const auto dir = Sys::U8Path(target).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        bool replaceFailed = false;
        if (!Sys::WriteFileBytesAtomic(target, text.data(), text.size(), &replaceFailed))
        {
            why = "cannot write " + target + (replaceFailed ? " (it is in use)" : "");
            return false;
        }
        Resource::AssetGuidIndex::Get().EnsureGuid(path);
        UI::InvalidateUIAsset(path);
        if (nodes) *nodes = back.NodeCount();
        return true;
    }
}

const char* UIEditorWindow::TabKindName(TabKind k)
{
    switch (k)
    {
    case TabKind::Asset:    return "asset";
    case TabKind::Template: return "template";
    default:                return "scene";
    }
}

UIEditorWindow::Tab* UIEditorWindow::TabByUid(std::uint32_t uid)
{
    for (Tab& t : m_tabs)
        if (t.uid == uid) return &t;
    return nullptr;
}

int UIEditorWindow::TabIndexByPath(const std::string& path) const
{
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (m_tabs[i].kind != TabKind::Scene && m_tabs[i].path == path) return i;
    return -1;
}

Entity UIEditorWindow::Sel(const UIEditorContext& ctx) const
{
    const Tab* t = ActiveTabPtr();
    if (!t || t->kind == TabKind::Scene) return ctx.selected;
    return (ctx.world && t->sel != NullEntity && ctx.world->IsAlive(t->sel)) ? t->sel : NullEntity;
}

void UIEditorWindow::SelectNode(const UIEditorContext& ctx, Entity e)
{
    Tab* t = ActiveTabPtr();
    if (t && t->kind != TabKind::Scene)
    {
        t->sel = e;
        return;
    }
    if (ctx.select) ctx.select(e);
}

bool UIEditorWindow::Frozen(const UIEditorContext& ctx) const
{
    if (ctx.playing) return true;
    const Tab* t = ActiveTabPtr();
    return t && (t->kind == TabKind::Template || t->parked);
}

bool UIEditorWindow::InstanceNode(const UIEditorContext& ctx, Entity e, std::string* why) const
{
    if (!ctx.world) return false;
    const Entity host = UI::DocumentHostOf(*ctx.world, e);
    if (host == NullEntity) return false;
    if (why)
    {
        const UI::UIDocument* d = ctx.world->GetComponent<UI::UIDocument>(host);
        *why = "'" + ctx.world->GetName(e) + "' is a node of the UI Document on '" + ctx.world->GetName(host) +
               "' - made from " + (d ? d->path : std::string("its .iui")) + " at run time: edit that asset instead";
    }
    return true;
}

std::string UIEditorWindow::FrozenWhy(const UIEditorContext& ctx) const
{
    const Tab* t = ActiveTabPtr();
    if (t && t->kind == TabKind::Template)
        return "a .ipfb UI template is read-only: Convert to UI Asset (a .iui) to edit it";
    return "Play mode: the designer is read-only (edit after Stop)";
}

void UIEditorWindow::Retag(World& w, const Tab& t) const
{
    if (t.subject == NullEntity || !w.IsAlive(t.subject)) return;
    // Every node of the stage, whatever made it: a Duplicate clones through the
    // serializers and a palette create through UITemplates - neither carries the tags.
    std::vector<Entity> stack{ t.subject }, kids;
    std::unordered_set<Entity> seen;
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!seen.insert(e).second || !w.IsAlive(e)) continue;
        if (!w.HasComponent<SceneTransientTag>(e)) w.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
        const PreviewStageTag* st = w.GetComponent<PreviewStageTag>(e);
        if (!st || st->owner != PreviewStageTag::kUIEditor)
            w.AddComponent<PreviewStageTag>(e, PreviewStageTag{ PreviewStageTag::kUIEditor });
        UI::UIChildrenOf(w, e, kids);
        stack.insert(stack.end(), kids.begin(), kids.end());
    }
    // A canvas asset would be laid out and drawn by the game screen too.
    // -uieditor_offscreen_off (uieditor_stage_isolation's reverse arm) skips this.
    if (w.HasComponent<UI::UICanvas>(t.subject) && !w.HasComponent<UI::UICanvasOffscreen>(t.subject) &&
        !m_harnessOffscreenOff)
        w.AddComponent<UI::UICanvasOffscreen>(t.subject, UI::UICanvasOffscreen{ UI::UICanvasOffscreen::kUIEditor });
}

bool UIEditorWindow::CaptureStage(World& w, const Tab& t, std::string& text, std::string* why,
                                  std::vector<std::pair<Entity, std::string>>* ids) const
{
    if (t.subject == NullEntity || !w.IsAlive(t.subject))
    {
        if (why) *why = "the tab has no stage";
        return false;
    }
    UI::UIAsset a;
    if (!UI::CaptureUIAsset(w, t.subject, a, why, nullptr, ids)) return false;
    text = UI::WriteUIAsset(a);
    return true;
}

bool UIEditorWindow::SpawnStage(const UIEditorContext& ctx, Tab& t, const std::string& text, std::string& why)
{
    if (!ctx.world) { why = "the editor has no world"; return false; }
    World& w = *ctx.world;
    UI::UIAsset a;
    std::vector<UI::UIAssetIssue> issues;
    if (!UI::ParseUIAsset(text, a, issues))
    {
        why = IssueText(t.path, issues);
        return false;
    }
    // A fresh seed every time: a stage left behind by a stray destroy (a root alone -
    // DestroyEntity is flat) keeps its GUIDs, and a new one must not resolve to it.
    t.seed = ECS::Guid::Generate();
    UI::UISpawnOptions o;
    o.seed     = t.seed;
    o.assetKey = t.path;
    o.assets   = ctx.assets;
    const Entity root = UI::SpawnUIAsset(w, a, o, &why);
    if (root == NullEntity) return false;
    t.subject = root;
    t.epoch   = w.Epoch();
    t.guid    = {};
    if (const GuidComponent* gc = w.GetComponent<GuidComponent>(root)) t.guid = gc->guid;
    t.parked  = false;
    Retag(w, t);
    // New ids: the designer-only flags start over; the selection is found by its node id.
    t.designHidden.clear();
    t.locked.clear();
    t.folded.clear();
    t.sel = t.selId.empty() ? NullEntity : UI::FindUINode(w, root, t.selId);
    std::string captured;
    if (CaptureStage(w, t, captured, nullptr)) t.stagedText = std::move(captured);
    return true;
}

void UIEditorWindow::DestroyStage(World& w, Tab& t)
{
    if (t.subject != NullEntity && w.IsAlive(t.subject))
    {
        const GuidComponent* gc = w.GetComponent<GuidComponent>(t.subject);
        if (!t.guid.IsValid() || (gc && gc->guid == t.guid)) Hierarchy::DestroySubtree(w, t.subject);
    }
    t.subject = NullEntity;
}

void UIEditorWindow::OnStageChanged(std::uint32_t uid, World* world)
{
    Tab* t = TabByUid(uid);
    if (!t || !world || t->subject == NullEntity || !world->IsAlive(t->subject)) return;
    Retag(*world, *t);
    std::vector<std::pair<Entity, std::string>> ids;
    std::string text;
    if (CaptureStage(*world, *t, text, nullptr, &ids))
    {
        t->stagedText = std::move(text);
        t->selId      = IdOf(ids, t->sel);
    }
}

bool UIEditorWindow::RevalidateStage(const UIEditorContext& ctx, Tab& t, std::string& why)
{
    World& w = *ctx.world;
    auto mine = [&](Entity e)
    {
        if (e == NullEntity || !w.IsAlive(e) || !t.guid.IsValid()) return false;
        const GuidComponent* gc = w.GetComponent<GuidComponent>(e);
        return gc && gc->guid == t.guid;
    };
    if (!t.parked && t.epoch == w.Epoch() && mine(t.subject)) return true;
    if (t.parked)
    {
        // Set aside for Play (TearDownForPlay): nothing to do until Stop.
        if (ctx.playing) return true;
        // Stop: the detached stage attached again - the same ids and generations, so
        // the ring's steps still name it.
        if (const std::shared_ptr<Undo::EntitySubtreeSnapshot> snap = std::move(t.parkedStage))
        {
            // Only into free ids: Attach skips an id that is alive and then restores the
            // node's components INTO it - over whatever holds that id now. Parked ids
            // should never be taken; if one is, the world moved on: spawn again.
            bool clash = false;
            for (const auto& n : snap->nodes) clash = clash || w.IsAlive(n.id);
            if (!clash)
            {
                snap->Attach(w);
                if (mine(snap->rootId))
                {
                    t.subject = snap->rootId;
                    t.epoch   = w.Epoch();
                    t.parked  = false;
                    Retag(w, t);
                    t.sel = t.selId.empty() ? NullEntity : UI::FindUINode(w, t.subject, t.selId);
                    LOG_INFO("[UIEDITOR] tab '%s': stage back after Play (%zu node(s), the same ids; its undo kept)",
                             t.name.c_str(), snap->nodes.size());
                    return true;
                }
                for (const auto& n : snap->nodes)   // attached, but not this tab's stage: out again
                    if (w.IsAlive(n.id)) w.DestroyEntity(n.id);
            }
            else
                for (const auto& n : snap->nodes)
                    if (!w.IsAlive(n.id)) w.UnreserveId(n.id);
            LOG_WARNING("[UIEDITOR] tab '%s': the stage could not be attached after Play (%s) - spawned again from "
                        "its capture (its undo starts over)", t.name.c_str(),
                        clash ? "an id it held is taken" : "its root is not the stage's");
        }
    }
    else
    {
        // The world changed under a live stage: the same stage - and, when its root
        // kept its id, the same ids (an epoch bump alone) - else the ring's steps
        // name entities that are gone.
        const Entity e = t.guid.IsValid() ? ECS::GuidRegistry::Get().Find(t.guid) : NullEntity;
        if (mine(e))
        {
            if (e != t.subject || t.epoch != w.Epoch())
            {
                if (e != t.subject && t.sink) t.sink->history.Clear();
                if (e != t.subject)
                {
                    t.designHidden.clear();
                    t.locked.clear();
                    t.folded.clear();
                    t.sel = t.selId.empty() ? NullEntity : UI::FindUINode(w, e, t.selId);
                }
                t.subject = e;
                t.epoch   = w.Epoch();
            }
            return true;
        }
        if (ctx.playing)
        {
            // A stage the played world lost: nothing to show until Stop.
            t.parked = true;
            return true;
        }
    }
    if (t.stagedText.empty())
    {
        why = "its stage is gone and nothing was captured to spawn it again";
        return false;
    }
    if (!SpawnStage(ctx, t, t.stagedText, why)) return false;
    if (t.sink) t.sink->history.Clear();
    LOG_INFO("[UIEDITOR] tab '%s': stage spawned again from its last capture (world epoch %u, %s)", t.name.c_str(),
             w.Epoch(), t.kind == TabKind::Asset && t.stagedText != t.savedText ? "unsaved edits kept" : "clean");
    return true;
}

void UIEditorWindow::DefaultFrame(World& w, Tab& t) const
{
    t.view.frame = UIView::RootFrame{};
    // A row template (a point-anchored root with a size of its own) previews at that
    // size, centred - a Settings row is 640 x 34; a stretched root fills the screen.
    if (w.HasComponent<UI::UICanvas>(t.subject)) return;
    if (const UI::UIRect* r = w.GetComponent<UI::UIRect>(t.subject))
        if (r->anchorMin.x == r->anchorMax.x && r->anchorMin.y == r->anchorMax.y && r->size.x > 0.f && r->size.y > 0.f)
        {
            t.view.frame.mode = UIView::RootFrame::Mode::Custom;
            t.view.frame.size = r->size;
        }
}

bool UIEditorWindow::IsUITemplateFile(const std::string& path)
{
    const std::string p = OnDisk(CanonAsset(path));
    return !p.empty() && IsUIPrefabFile(Sys::U8Path(p));
}

void UIEditorWindow::RequestOpenAsset(const std::string& path)
{
    m_pendingOpenAssets.push_back(path);
    m_open = true;
}

bool UIEditorWindow::OpenAssetNow(const UIEditorContext& ctx, const std::string& pathIn, std::string& why, int* tabOut)
{
    if (!ctx.world) { why = "the editor has no world"; return false; }
    const std::string path = CanonAsset(pathIn);
    const bool iui = EndsWithCI(path, ".iui"), ipfb = EndsWithCI(path, ".ipfb");
    if (!iui && !ipfb)
    {
        why = "'" + pathIn + "' is no .iui UI asset and no .ipfb UI template";
        return false;
    }
    // Never under Draw's Tab* (a push_back may move every tab): the PPA opens it.
    if (m_inDraw)
    {
        RequestOpenAsset(path);
        if (tabOut) *tabOut = -1;
        return true;
    }
    if (const int open = TabIndexByPath(path); open >= 0)
    {
        Activate(open);
        m_open        = true;
        m_focusWindow = true;
        if (tabOut) *tabOut = open;
        return true;
    }
    if (ctx.playing) { why = "Play mode: open a UI asset after Stop"; return false; }
    if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
    {
        why = "already " + std::to_string(kMaxTabs) + " tabs open - close one";
        return false;
    }
    World& w = *ctx.world;
    Tab t;
    t.uid  = m_nextUid++;
    t.kind = iui ? TabKind::Asset : TabKind::Template;
    t.path = path;
    t.name = BaseName(path);
    if (iui)
    {
        std::string raw;
        if (!Resource::AssetFS::Get().ReadFileText(path, raw)) { why = "cannot read " + path; return false; }
        UI::UIAsset a;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(raw, a, issues)) { why = IssueText(path, issues); return false; }
        if (!SpawnStage(ctx, t, UI::WriteUIAsset(a), why)) return false;
        // Clean = the file as the stage holds it: what a Save of the untouched stage writes.
        t.savedText = t.stagedText;
        std::string target, roWhy;
        if (!WritableTarget(path, target, roWhy)) t.readOnlyWhy = roWhy;
        // Its own ring: EditorLayer routes every edit of the stage here (ActiveSink).
        t.sink = std::make_shared<Undo::EditSink>();
        const std::uint32_t uid = t.uid;
        World* world = ctx.world;
        t.sink->changed = [this, uid, world] { OnStageChanged(uid, world); };
        t.sink->history.Context().setSelection = [this, uid](Entity e)
        {
            if (Tab* tt = TabByUid(uid)) tt->sel = e;
        };
    }
    else
    {
        if (!IsUITemplateFile(path)) { why = "'" + path + "' is no UI template (its root is no UI node)"; return false; }
        if (!ctx.loadPrefab) { why = "this editor cannot load prefabs"; return false; }
        const Entity root = ctx.loadPrefab(path, &why);
        if (root == NullEntity) return false;
        if (!UI::IsUINode(w, root))
        {
            Hierarchy::DestroySubtree(w, root);
            why = "'" + path + "' did not spawn a UI node";
            return false;
        }
        // Shown as it is, linked to nothing: no prefab instance, no parent it was
        // authored under (a row template names one that is not here).
        if (w.HasComponent<PrefabSourcePath>(root)) w.RemoveComponent<PrefabSourcePath>(root);
        if (w.HasComponent<UI::UIParent>(root)) w.RemoveComponent<UI::UIParent>(root);
        t.subject = root;
        t.epoch   = w.Epoch();
        if (const GuidComponent* gc = w.GetComponent<GuidComponent>(root)) t.guid = gc->guid;
        Retag(w, t);
        if (!CaptureStage(w, t, t.stagedText, &why))
        {
            Hierarchy::DestroySubtree(w, root);
            return false;
        }
        t.readOnlyWhy = "a .ipfb UI template is read-only: Convert to UI Asset (a .iui) to edit it";
    }
    StatFile(path, t.diskStamp, t.diskSize);
    DefaultFrame(w, t);
    DesignSize(ctx, t, t.designW, t.designH);
    m_tabs.push_back(std::move(t));
    const int tab = static_cast<int>(m_tabs.size()) - 1;
    const Tab& n  = m_tabs.back();
    UI::UIAsset staged;
    std::vector<UI::UIAssetIssue> unused;
    UI::ParseUIAsset(n.stagedText, staged, unused);
    LOG_INFO("[UIEDITOR] open tab='%s' kind=%s path=%s subject=%u nodes=%zu frame=%s%s%s", n.name.c_str(),
             TabKindName(n.kind), n.path.c_str(), static_cast<unsigned>(n.subject), staged.NodeCount(),
             FrameText(n.view.frame).c_str(), n.readOnlyWhy.empty() ? "" : " read-only: ", n.readOnlyWhy.c_str());
    m_active      = tab;
    m_focusTab    = tab;
    m_open        = true;
    m_focusWindow = true;
    if (tabOut) *tabOut = tab;
    return true;
}

std::shared_ptr<Undo::EditSink> UIEditorWindow::ActiveSink() const
{
    const Tab* t = ActiveTabPtr();
    if (!t || t->kind != TabKind::Asset || t->parked || t->subject == NullEntity) return nullptr;
    return t->sink;
}

std::shared_ptr<Undo::EditSink> UIEditorWindow::SinkForStageEntity(World& w, Entity e) const
{
    Entity root = e;
    for (int guard = 0; guard < 4096; ++guard)
    {
        const Entity p = UI::ResolveUIParent(w, root);
        if (p == NullEntity || p == root) break;
        root = p;
    }
    for (const Tab& t : m_tabs)
        if (t.kind == TabKind::Asset && !t.parked && t.subject == root) return t.sink;
    return nullptr;
}

bool UIEditorWindow::EditsEntity(Entity e) const
{
    const Tab* t = ActiveTabPtr();
    return e != NullEntity && t && t->kind != TabKind::Scene && t->sel == e;
}

bool UIEditorWindow::OwnsShortcuts() const
{
    const Tab* t = ActiveTabPtr();
    return m_open && m_focused && t && t->kind != TabKind::Scene;
}

void UIEditorWindow::RequestSave(bool saveAs)
{
    const Tab* t = ActiveTabPtr();
    if (!t || t->kind == TabKind::Scene) return;
    if (!saveAs)
    {
        m_pendingSave = 1;
        return;
    }
    // The dialog belongs to the UI handler; the write to the PPA. (A template's Save
    // As is its Convert.)
    std::string dir = OnDisk(t->path);
    dir = Sys::PathToUtf8(Sys::U8Path(dir.empty() ? t->path : dir).parent_path());
    const std::string p = SaveFileDialog(kIuiFilter, "iui", dir.c_str());
    if (p.empty()) return;
    m_pendingSave   = 2;
    m_pendingSaveAs = p;
}

void UIEditorWindow::RequestUndo(bool redo) { m_pendingUndo += redo ? 1 : -1; }
void UIEditorWindow::RequestDuplicate() { m_pendingDuplicate = true; }

bool UIEditorWindow::SaveTab(const UIEditorContext& ctx, Tab& t, const std::string& asPathIn, std::string& why,
                             std::size_t* bytes, std::size_t* nodes, bool overwrite)
{
    if (t.kind == TabKind::Scene)
    {
        why = "a scene tab's canvas is saved with its scene (File > Save Scene)";
        return false;
    }
    if (t.kind == TabKind::Template) { why = t.readOnlyWhy; return false; }
    if (!ctx.world) { why = "the editor has no world"; return false; }
    World& w = *ctx.world;
    const std::string asPath = asPathIn.empty() ? t.path : CanonAsset(asPathIn);
    if (!EndsWithCI(asPath, ".iui")) { why = "'" + asPathIn + "': a UI asset is a .iui"; return false; }
    const bool saveAs = asPath != t.path;
    if (saveAs && TabIndexByPath(asPath) >= 0) { why = asPath + " is open in another tab"; return false; }
    // Like Convert / Save Branch / Extract: another existing file only when asked.
    if (saveAs && !overwrite && !OnDisk(asPath).empty())
    {
        why = asPath + " exists ({overwrite = true} replaces it)";
        return false;
    }
    if (!saveAs && !t.readOnlyWhy.empty()) { why = t.readOnlyWhy; return false; }
    // The stage as it is now - or, parked for Play, its last capture.
    std::string text;
    std::vector<std::pair<Entity, std::string>> ids;
    if (t.subject != NullEntity && w.IsAlive(t.subject))
    {
        if (!CaptureStage(w, t, text, &why, &ids)) return false;
    }
    else if (!t.stagedText.empty())
        text = t.stagedText;
    else
    {
        why = "the tab has no stage";
        return false;
    }
    std::size_t n = 0;
    if (!WriteIui(asPath, text, why, &n)) return false;
    // A node made here had no id of its own: it keeps the one the file now holds, so
    // renaming it later does not rename it in the file.
    for (const auto& [e, id] : ids)
        if (w.IsAlive(e) && !w.HasComponent<UI::UIAssetNodeId>(e)) w.AddComponent<UI::UIAssetNodeId>(e, UI::UIAssetNodeId{ id });
    t.savedText = t.stagedText = text;
    t.selId       = IdOf(ids, t.sel);
    t.diskChanged = false;
    StatFile(asPath, t.diskStamp, t.diskSize);
    if (saveAs)
    {
        t.path = asPath;
        t.name = BaseName(asPath);
        t.readOnlyWhy.clear();
    }
    if (bytes) *bytes = text.size();
    if (nodes) *nodes = n;
    LOG_INFO("[UIEDITOR] save tab='%s' path=%s bytes=%zu nodes=%zu%s", t.name.c_str(), asPath.c_str(), text.size(), n,
             saveAs ? " (save as)" : "");
    return true;
}

bool UIEditorWindow::Save(const UIEditorContext& ctx, const std::string& asPath, std::string& why, std::size_t* bytes,
                          std::size_t* nodes, bool overwrite)
{
    Tab* t = ActiveTabPtr();
    if (!t) { why = "no tab open"; return false; }
    return SaveTab(ctx, *t, asPath, why, bytes, nodes, overwrite);
}

bool UIEditorWindow::ReloadTab(const UIEditorContext& ctx, Tab& t, std::string& why)
{
    if (t.kind == TabKind::Scene) { why = "a scene tab has no file of its own"; return false; }
    if (ctx.playing || !ctx.world) { why = "Play mode: revert after Stop"; return false; }
    World& w = *ctx.world;
    if (t.kind == TabKind::Asset)
    {
        UI::InvalidateUIAsset(t.path);
        std::string raw;
        if (!Resource::AssetFS::Get().ReadFileText(t.path, raw)) { why = "cannot read " + t.path; return false; }
        UI::UIAsset a;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(raw, a, issues)) { why = IssueText(t.path, issues); return false; }
        DestroyStage(w, t);
        if (!SpawnStage(ctx, t, UI::WriteUIAsset(a), why)) return false;
        t.savedText = t.stagedText;
    }
    else
    {
        // A template: the .ipfb again, as OpenAssetNow shows it.
        if (!ctx.loadPrefab) { why = "this editor cannot load prefabs"; return false; }
        const Entity root = ctx.loadPrefab(t.path, &why);
        if (root == NullEntity) return false;
        DestroyStage(w, t);
        if (w.HasComponent<PrefabSourcePath>(root)) w.RemoveComponent<PrefabSourcePath>(root);
        if (w.HasComponent<UI::UIParent>(root)) w.RemoveComponent<UI::UIParent>(root);
        t.subject = root;
        t.epoch   = w.Epoch();
        t.guid    = {};
        if (const GuidComponent* gc = w.GetComponent<GuidComponent>(root)) t.guid = gc->guid;
        Retag(w, t);
        CaptureStage(w, t, t.stagedText, nullptr);
    }
    if (t.sink) t.sink->history.Clear();
    t.diskChanged = false;
    StatFile(t.path, t.diskStamp, t.diskSize);
    RebuildRows(ctx);
    LOG_INFO("[UIEDITOR] reload tab='%s' path=%s", t.name.c_str(), t.path.c_str());
    return true;
}

bool UIEditorWindow::Revert(const UIEditorContext& ctx, std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t) { why = "no tab open"; return false; }
    return ReloadTab(ctx, *t, why);
}

void UIEditorWindow::PollDisk(const UIEditorContext& ctx, Tab& t)
{
    if (t.kind == TabKind::Scene || t.parked) return;
    std::int64_t  stamp = 0;
    std::uint64_t size  = 0;
    if (!StatFile(t.path, stamp, size)) return;   // gone: the tab keeps it (Save writes it again)
    if (stamp == t.diskStamp && size == t.diskSize) return;
    if (t.kind == TabKind::Asset && t.stagedText != t.savedText)
    {
        // Unsaved edits are never thrown away under the person: say it, once per change.
        LOG_WARNING("[UIEDITOR] tab '%s': %s changed on disk under unsaved edits (Revert takes the file, Save "
                    "writes yours over it)", t.name.c_str(), t.path.c_str());
        t.diskChanged = true;
        t.diskStamp   = stamp;
        t.diskSize    = size;
        return;
    }
    std::string why;
    if (ReloadTab(ctx, t, why))
        LOG_INFO("[UIEDITOR] tab '%s' follows %s: it changed on disk", t.name.c_str(), t.path.c_str());
    else
    {
        LOG_WARNING("[UIEDITOR] tab '%s': %s changed on disk and does not load: %s", t.name.c_str(), t.path.c_str(),
                    why.c_str());
        t.diskStamp = stamp;
        t.diskSize  = size;
    }
}

bool UIEditorWindow::Undo(const UIEditorContext& ctx, bool redo, std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t) { why = "no tab open"; return false; }
    if (t->kind == TabKind::Scene)
    {
        why = "a scene tab's edits are steps of the scene's history: Editor.Undo() / Editor.Redo()";
        return false;
    }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    if (!ctx.sinkUndo || !ctx.sinkUndo(redo))
    {
        why = redo ? "nothing to redo" : "nothing to undo";
        return false;
    }
    RebuildRows(ctx);
    const Tab* a = ActiveTabPtr();
    LOG_INFO("[UIEDITOR] %s tab='%s' (undo %zu, redo %zu)", redo ? "redo" : "undo", a->name.c_str(),
             a->sink ? a->sink->history.UndoCount() : 0, a->sink ? a->sink->history.RedoCount() : 0);
    return true;
}

Entity UIEditorWindow::Duplicate(const UIEditorContext& ctx, Entity e, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return NullEntity; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return NullEntity; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open tab"; return NullEntity; }
    if (r->isRoot) { why = "the root has no place beside itself"; return NullEntity; }
    if (InstanceNode(ctx, e, &why)) return NullEntity;
    if (!ctx.duplicate) { why = "this editor cannot duplicate"; return NullEntity; }
    const std::string name = ctx.world->GetName(e);
    const Entity c = ctx.duplicate(e);
    if (c == NullEntity) { why = "the copy failed (see the log)"; return NullEntity; }
    SelectNode(ctx, c);
    RebuildRows(ctx);
    LOG_INFO("[UIEDITOR] duplicate %s -> entity %u", name.c_str(), static_cast<unsigned>(c));
    return c;
}

bool UIEditorWindow::Convert(const UIEditorContext& ctx, const std::string& pathIn, bool overwrite, std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t || t->kind != TabKind::Template) { why = "the active tab is not a .ipfb UI template"; return false; }
    if (!ctx.world) { why = "the editor has no world"; return false; }
    const std::string path = CanonAsset(pathIn);
    if (!EndsWithCI(path, ".iui")) { why = "'" + pathIn + "': a UI asset is a .iui"; return false; }
    if (TabIndexByPath(path) >= 0) { why = path + " is open in a tab"; return false; }
    if (!overwrite && !OnDisk(path).empty()) { why = path + " exists ({overwrite = true} replaces it)"; return false; }
    std::string text;
    if (!CaptureStage(*ctx.world, *t, text, &why)) return false;
    std::size_t n = 0;
    if (!WriteIui(path, text, why, &n)) return false;
    const std::uint32_t from = t->uid;
    const std::string   src  = t->path;
    LOG_INFO("[UIEDITOR] convert %s -> %s bytes=%zu nodes=%zu", src.c_str(), path.c_str(), text.size(), n);
    // The .iui in a tab of its own; the template's closes.
    if (!OpenAssetNow(ctx, path, why)) return false;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (m_tabs[i].uid == from)
        {
            CloseTab(ctx, i, true);
            break;
        }
    Activate(TabIndexByPath(path));
    return true;
}

bool UIEditorWindow::SaveBranch(const UIEditorContext& ctx, Entity e, const std::string& pathIn, bool overwrite, bool open,
                                std::string& why, std::size_t* nodes)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    RebuildRows(ctx);
    if (!FindRow(e)) { why = "not a node of the open tab"; return false; }
    const std::string path = CanonAsset(pathIn);
    if (!EndsWithCI(path, ".iui")) { why = "'" + pathIn + "': a UI asset is a .iui"; return false; }
    if (TabIndexByPath(path) >= 0) { why = path + " is open in a tab"; return false; }
    if (!overwrite && !OnDisk(path).empty()) { why = path + " exists ({overwrite = true} replaces it)"; return false; }
    UI::UIAsset a;
    if (!UI::CaptureUIAsset(*ctx.world, e, a, &why)) return false;
    const std::string text = UI::WriteUIAsset(a);
    std::size_t n = 0;
    if (!WriteIui(path, text, why, &n)) return false;
    if (nodes) *nodes = n;
    LOG_INFO("[UIEDITOR] save branch '%s' -> %s bytes=%zu nodes=%zu", ctx.world->GetName(e).c_str(), path.c_str(),
             text.size(), n);
    // A .iui (v1) holds no references - say what was written inline.
    if (const std::size_t docs = UI::DocumentNodesUnder(*ctx.world, e))
        LOG_WARNING("[UIEDITOR] save branch '%s': %zu node(s) of UI Documents written inline - %s holds no reference to "
                    "their .iui", ctx.world->GetName(e).c_str(), docs, path.c_str());
    if (open)
    {
        std::string openWhy;
        if (!OpenAssetNow(ctx, path, openWhy))
            LOG_WARNING("[UIEDITOR] saved %s, but it does not open: %s", path.c_str(), openWhy.c_str());
    }
    return true;
}

Entity UIEditorWindow::Extract(const UIEditorContext& ctx, Entity e, const std::string& pathIn, bool overwrite,
                               std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) { why = "no canvas open"; return NullEntity; }
    if (t->kind != TabKind::Scene) { why = "Extract to UI Asset is a scene tab's (this tab is a file already)"; return NullEntity; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return NullEntity; }
    if (InstanceNode(ctx, e, &why)) return NullEntity;
    World& w = *ctx.world;
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return NullEntity; }
    if (!ctx.recordSpawn || !ctx.deleteEntity || !ctx.moveUINode) { why = "this editor cannot restructure UI"; return NullEntity; }
    const std::string path = CanonAsset(pathIn);
    if (!EndsWithCI(path, ".iui")) { why = "'" + pathIn + "': a UI asset is a .iui"; return NullEntity; }
    if (TabIndexByPath(path) >= 0) { why = path + " is open in a tab"; return NullEntity; }
    if (!overwrite && !OnDisk(path).empty()) { why = path + " exists ({overwrite = true} replaces it)"; return NullEntity; }
    const bool canvas = w.HasComponent<UI::UICanvas>(e);
    UI::UIAsset a;
    if (!UI::CaptureUIAsset(w, e, a, &why)) return NullEntity;
    const std::string text = UI::WriteUIAsset(a);
    std::size_t n = 0;
    const bool existed = !OnDisk(path).empty();   // a failed Extract leaves no new file behind
    if (!WriteIui(path, text, why, &n)) return NullEntity;
    if (const std::size_t docs = UI::DocumentNodesUnder(w, e))
        LOG_WARNING("[UIEDITOR] extract '%s': %zu node(s) of UI Documents written inline - %s holds no reference to "
                    "their .iui", w.GetName(e).c_str(), docs, path.c_str());

    // The scene's side, one undo step: a host where the branch was (a rect host fills
    // the branch's parent, so the instance - the branch's own rect - lands where the
    // branch was; a canvas's host is an entity of its own), then the branch goes.
    const std::string name   = w.GetName(e);
    const Entity      parent = r->parent;
    int at = 0;
    if (!canvas)
    {
        std::vector<Entity> sibs;
        UI::UIChildrenOf(w, parent, sibs);
        at = static_cast<int>(std::find(sibs.begin(), sibs.end(), e) - sibs.begin());
    }
    Entity host = NullEntity;
    if (ctx.beginMacro) ctx.beginMacro("Extract " + name + " to UI Asset");
    ctx.recordSpawn("UI Document", [&]
    {
        if (canvas)
            host = w.CreateEntity();
        else
        {
            host = UITemplates::Create(w, UITemplates::Widget::EmptyRect, parent);
            if (host == NullEntity) return;
            UI::UIRect* hr = w.GetComponent<UI::UIRect>(host);
            const int sib  = hr->siblingIndex;
            *hr = UI::UIRect{};
            hr->anchorMin    = { 0.f, 0.f };
            hr->anchorMax    = { 1.f, 1.f };
            hr->pivot        = { 0.5f, 0.5f };
            hr->size         = { 0.f, 0.f };
            hr->offset       = { 0.f, 0.f };
            hr->siblingIndex = sib;
        }
        // The instance keeps the branch's name (a script finds it as before); the host
        // says what it is.
        w.SetName(host, name + " Document");
        // A GUID of its own: it seeds the instance's node GUIDs, the same at every load.
        ECS::EnsureGuidOn(w, host);
        UI::UIDocument d;
        d.path = path;
        w.AddComponent<UI::UIDocument>(host, std::move(d));
    });
    bool ok = host != NullEntity;
    if (ok && !canvas) ok = ctx.moveUINode(host, parent, at, &why);
    if (ok) ctx.deleteEntity(e);
    if (ctx.endMacro) ctx.endMacro();
    if (!ok)
    {
        if (why.empty()) why = "the host could not be made";
        if (!existed)
        {
            // Nothing references the file it just wrote (nor its .meta): not left behind.
            if (const std::string written = OnDisk(path); !written.empty())
            {
                std::error_code ec;
                std::filesystem::remove(Sys::U8Path(written), ec);
                std::filesystem::remove(Sys::U8Path(written + ".meta"), ec);
            }
            UI::InvalidateUIAsset(path);
        }
        return NullEntity;
    }
    // The instance now, not next frame: the picture (and a script) sees it at once.
    UI::UpdateDocuments(w);
    SelectNode(ctx, host);
    RebuildRows(ctx);
    LOG_INFO("[UIEDITOR] extract '%s' -> %s nodes=%zu host=%u%s", name.c_str(), path.c_str(), n,
             static_cast<unsigned>(host), canvas ? " (a canvas: its host is an entity of its own)" : "");
    // A canvas was this tab's subject: its tab closes with it (next PPA); the asset
    // opens in a tab of its own, where it is edited now.
    if (canvas)
    {
        std::string openWhy;
        if (!OpenAssetNow(ctx, path, openWhy))
            LOG_WARNING("[UIEDITOR] extracted %s, but it does not open: %s", path.c_str(), openWhy.c_str());
    }
    return host;
}

std::vector<std::string> UIEditorWindow::UnsavedAssets() const
{
    std::vector<std::string> out;
    for (const Tab& t : m_tabs)
        if (t.kind == TabKind::Asset && t.stagedText != t.savedText) out.push_back(t.path);
    return out;
}

bool UIEditorWindow::SaveAllUnsaved(const UIEditorContext& ctx, std::string& why)
{
    for (Tab& t : m_tabs)
        if (t.kind == TabKind::Asset && t.stagedText != t.savedText &&
            !SaveTab(ctx, t, std::string(), why, nullptr, nullptr))
        {
            why = t.path + ": " + why;
            return false;
        }
    return true;
}

void UIEditorWindow::TearDownForPlay(Renderer* renderer, World* world)
{
    (void)renderer;
    if (!world) return;
    for (Tab& t : m_tabs)
    {
        if (t.kind == TabKind::Scene || t.parked) continue;
        std::size_t nodes = 0;
        if (t.subject != NullEntity && world->IsAlive(t.subject))
        {
            // The capture first: the fallback if the attach after Stop cannot happen.
            std::vector<std::pair<Entity, std::string>> ids;
            std::string text;
            if (CaptureStage(*world, t, text, nullptr, &ids))
            {
                t.stagedText = std::move(text);
                t.selId      = IdOf(ids, t.sel);
            }
            // Detach, as an undo of a create does: destroyed, its ids parked (CloneState
            // carries the parking into the played world and back) - so the same ids,
            // generations and the ring's steps are good again after Stop.
            auto snap = std::make_shared<Undo::EntitySubtreeSnapshot>(
                Undo::EntitySubtreeSnapshot::Capture(*world, t.subject));
            nodes = snap->nodes.size();
            snap->Detach(*world);
            t.parkedStage = std::move(snap);
        }
        t.sel    = NullEntity;
        t.parked = true;   // t.subject keeps the root's id: the attach brings it back
        LOG_INFO("[UIEDITOR] tab '%s': stage set aside for Play (%zu node(s) detached, ids parked)", t.name.c_str(),
                 nodes);
    }
    m_rows.clear();
    m_rowsTab = -1;
}

void UIEditorWindow::CommitGestures(const UIEditorContext& ctx)
{
    if (m_drag.active) EndDrag(ctx);
    if (m_nudge.active) EndNudge(ctx);
}

// ==== The description ====
void UIEditorWindow::DesignSize(const UIEditorContext& ctx, const Tab& t, float& w, float& h) const
{
    w = 1920.f;
    h = 1080.f;
    bool done = false;
    if (t.view.res == Resolution::Custom)
    {
        w = static_cast<float>(t.view.resW);
        h = static_cast<float>(t.view.resH);
        done = true;
    }
    else if (t.view.res == Resolution::MatchMain && m_mainScreen.x >= 1.f && m_mainScreen.y >= 1.f)
    {
        w = m_mainScreen.x;
        h = m_mainScreen.y;
        done = true;
    }
    if (!done && ctx.world)   // Reference (and MatchMain before the game has painted)
        if (const UI::UICanvas* cv = ctx.world->GetComponent<UI::UICanvas>(t.subject))
        {
            w = cv->referenceResolution.x;
            h = cv->referenceResolution.y;
        }
    w = std::clamp(std::round(w), kMinDesign, kMaxDesign);
    h = std::clamp(std::round(h), kMinDesign, kMaxDesign);
}

void UIEditorWindow::BoxSize(std::uint32_t& w, std::uint32_t& h) const
{
    w = m_pinW ? m_pinW : (m_outW ? m_outW : kDefaultBoxW);
    h = m_pinH ? m_pinH : (m_outH ? m_outH : kDefaultBoxH);
}

void UIEditorWindow::EffectiveView(const Tab& t, float dw, float dh, float& z, float pan[2]) const
{
    if (t.view.zoom > 0.f)
    {
        z = t.view.zoom;
        pan[0] = t.view.pan[0];
        pan[1] = t.view.pan[1];
        return;
    }
    // Fit: the whole frame in the visible canvas (the pinned box, else the canvas
    // the last Draw laid out), centred.
    std::uint32_t bw = 0, bh = 0;
    BoxSize(bw, bh);
    const float cw = m_pinW ? static_cast<float>(m_pinW) : (m_canvasW > 0.f ? m_canvasW : static_cast<float>(bw));
    const float ch = m_pinH ? static_cast<float>(m_pinH) : (m_canvasH > 0.f ? m_canvasH : static_cast<float>(bh));
    const float m  = (cw > 8.f * kFitMargin && ch > 8.f * kFitMargin) ? kFitMargin : 0.f;
    // A UI-node subject in a frame of its own (a row template) fits that frame -
    // its root as laid out - not the whole design screen (UMG's zoom to the widget).
    float fx = 0.f, fy = 0.f, fw = dw, fh = dh;
    if (t.view.frame.mode == UIView::RootFrame::Mode::Custom && &t == ActiveTabPtr() && m_rowsTab == m_active)
        for (const Row& r : m_rows)
            if (r.isRoot && r.isRect && r.rect.mx.x > r.rect.mn.x && r.rect.mx.y > r.rect.mn.y)
            {
                fx = r.rect.mn.x;
                fy = r.rect.mn.y;
                fw = r.rect.mx.x - r.rect.mn.x;
                fh = r.rect.mx.y - r.rect.mn.y;
                break;
            }
    z = std::clamp((std::min)((cw - 2.f * m) / (std::max)(fw, 1.f), (ch - 2.f * m) / (std::max)(fh, 1.f)),
                   kMinZoom, kMaxZoom);
    pan[0] = std::floor((cw - fw * z) * 0.5f - fx * z);
    pan[1] = std::floor((ch - fh * z) * 0.5f - fy * z);
}

bool UIEditorWindow::BuildDesc(const UIEditorContext& ctx, const Tab& t, UIView::Desc& d) const
{
    if (!ctx.world) return false;
    float dw = 0.f, dh = 0.f;
    DesignSize(ctx, t, dw, dh);
    std::uint32_t bw = 0, bh = 0;
    BoxSize(bw, bh);
    float z = 1.f, pan[2] = { 0.f, 0.f };
    EffectiveView(t, dw, dh, z, pan);

    if (t.subject == NullEntity) return false;   // a stage set aside for Play
    d = UIView::Desc{};
    d.subject       = t.subject;
    d.worldEpoch    = t.epoch;
    d.frame         = t.view.frame;               // a UI-node subject's preview frame (a canvas ignores it)
    // The reverse arm of uieditor_parity: a design screen one pixel wider than
    // the one asked for - the layout, and so the hash, must move.
    d.designSize    = UI::Vec2{ dw + (m_harnessParitySkew ? 1.f : 0.f), dh };
    d.targetW       = bw;
    d.targetH       = bh;
    d.zoom          = z;
    d.pan           = UI::Vec2{ pan[0], pan[1] };
    d.background    = t.view.background;
    d.solidRgba     = t.view.solidRgba;
    d.workspaceRgba = kWorkspaceRgba;
    d.clipToFrame   = true;
    d.state         = t.view.state;
    // Every interactable, or only the selection (UMG's preview of one widget's state).
    d.stateTarget   = (t.view.stateSelected && t.view.state != UI::StatePreview::Live) ? Sel(ctx) : NullEntity;
    d.showHidden    = t.view.showHidden;
    d.realtime      = t.view.realtime;
    d.debug         = m_harnessDebug;
    d.skip          = t.designHidden;   // hidden in the designer
    return true;
}

bool UIEditorWindow::Layout(const UIEditorContext& ctx, std::vector<UI::LayoutNode>& out, float& scale,
                            UI::Vec2& designSize, std::string& why) const
{
    out.clear();
    scale = 1.f;
    const Tab* t = ActiveTabPtr();
    if (!t) { why = "no canvas open"; return false; }
    UIView::Desc d;
    if (!BuildDesc(ctx, *t, d)) { why = "the editor has no world"; return false; }
    designSize = d.designSize;
    return UIViewService::LayoutDesc(d, *ctx.world, out, scale, why);
}

// ==== Settling and shots ====
void UIEditorWindow::UpdateSettle(const UIView::Status& st)
{
    const bool shown = st.hasOutput && st.current && st.paintedSerial == m_descSerial;
    if (shown && !m_shown) m_shownSince = m_frame;
    m_shown = shown;
    // An image that never loads (a missing file) must not hold a shot forever:
    // after kLoadGrace frames the picture is taken as it is (the shot line says so).
    const bool complete = shown && (st.pendingImages == 0 || m_frame >= m_shownSince + kLoadGrace);
    if (complete && !m_complete) m_completeSince = m_frame;
    m_complete = complete;
}

bool UIEditorWindow::Settled() const
{
    return m_viewId != ~0u && m_complete && m_frame >= m_completeSince + kSettleFrames;
}

bool UIEditorWindow::WriteShot(const UIEditorContext& ctx, const std::string& path, std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t) { why = "no canvas open"; return false; }
    UIViewService* views = ctx.renderer ? ctx.renderer->GetUIViews() : nullptr;
    const RHI::Texture* out = (views && m_viewId != ~0u) ? views->Output(m_viewId) : nullptr;
    if (!out || !ctx.gfx) { why = "the picture has no target yet"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(Sys::U8Path(path).parent_path(), ec);
    if (!ctx.gfx->CaptureTextureToPNG(*out, RHI::ResourceState::SHADER_RESOURCE, path.c_str()))
    {
        why = "the capture failed (see the log)";
        return false;
    }
    const UIView::Status st = views->GetStatus(m_viewId);
    float z = 1.f, pan[2] = { 0.f, 0.f };
    EffectiveView(*t, t->designW, t->designH, z, pan);
    LOG_INFO("[UIEDITOR] shot %s: file='%s' w=%u h=%u design=%gx%g zoom=%.4f pan=%g:%g scale=%.4f nodes=%u cmds=%u "
             "verts=%u effects=%u hash=%016llx renders=%llu pendingImages=%u bg=%s tab=%s%s%s",
             BaseName(path).c_str(), path.c_str(), st.width, st.height, st.designSize.x, st.designSize.y, z, pan[0],
             pan[1], st.canvasScale, st.nodes, st.cmds, st.verts, st.effects,
             static_cast<unsigned long long>(st.designHash), static_cast<unsigned long long>(st.renders),
             st.pendingImages, BackgroundName(t->view.background), t->name.c_str(), st.why.empty() ? "" : " why=",
             st.why.c_str());
    return true;
}

std::uint64_t UIEditorWindow::QueueShot(const std::string& path, std::uint32_t requestId)
{
    const std::uint64_t job = Harness::Jobs::TryCreate("uieditor_shot", path, requestId);
    if (!job) return 0;
    Harness::Jobs::Start(job);
    m_shots.push_back({ path, job, m_frame });
    return job;
}

UIEditorWindow::StateView UIEditorWindow::State(const UIEditorContext& ctx) const
{
    StateView s;
    s.open       = m_open;
    s.active     = m_active;
    s.mainScreen = m_mainScreen;
    UIViewService* views = ctx.renderer ? ctx.renderer->GetUIViews() : nullptr;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
    {
        const Tab& t = m_tabs[i];
        TabState ts;
        ts.subject = t.parked ? NullEntity : t.subject;   // a parked stage is no entity now
        ts.name    = t.name;
        ts.guid    = HexGuid(t.guid);
        ts.view    = t.view;
        ts.designW = static_cast<std::uint32_t>(t.designW);
        ts.designH = static_cast<std::uint32_t>(t.designH);
        EffectiveView(t, t.designW, t.designH, ts.zoom, ts.pan);
        // A scene tab shares the editor's selection (when it is one of its nodes); an
        // asset / template tab has its own.
        const Entity sel = t.kind == TabKind::Scene ? ctx.selected : t.sel;
        if (ctx.world && sel != NullEntity && ctx.world->IsAlive(sel) && t.subject != NullEntity)
        {
            Entity cur = sel;
            for (int guard = 0; guard < 4096 && cur != NullEntity; ++guard)
            {
                if (cur == t.subject) { ts.selected = sel; break; }
                cur = UI::ResolveUIParent(*ctx.world, cur);
            }
        }
        ts.kind        = t.kind;
        ts.path        = t.path;
        ts.dirty       = t.kind == TabKind::Asset && t.stagedText != t.savedText;
        ts.readOnly    = !t.readOnlyWhy.empty();
        ts.readOnlyWhy = t.readOnlyWhy;
        ts.diskChanged = t.diskChanged;
        ts.parked      = t.parked;
        ts.undo        = t.sink ? t.sink->history.UndoCount() : 0;
        ts.redo        = t.sink ? t.sink->history.RedoCount() : 0;
        if (i == m_viewTab && m_viewId != ~0u && views)
        {
            ts.viewId  = m_viewId;
            ts.status  = views->GetStatus(m_viewId);
            ts.settled = Settled();
        }
        ts.designHidden = t.designHidden;
        ts.locked       = t.locked;
        s.tabs.push_back(std::move(ts));
    }
    return s;
}

// ==== Scripted surface ====
bool UIEditorWindow::Set(const std::string& keyIn, const std::string& value, std::string& why)
{
    const std::string key = Lower(keyIn);
    if (key == "size")
    {
        std::uint32_t w = 0, h = 0;
        if (!ParseSize(value, w, h) || ((w | h) != 0 && (w < 16 || h < 16 || w > UIViewService::kMaxSize ||
                                                         h > UIViewService::kMaxSize)))
        {
            why = "size: '" + value + "' is not WxH (16.." + std::to_string(UIViewService::kMaxSize) +
                  ", or 0x0 to follow the canvas)";
            return false;
        }
        PinSize(w, h);
        return true;
    }
    Tab* t = ActiveTabPtr();
    if (!t) { why = "no canvas open"; return false; }
    ViewState& v = t->view;
    if (key == "res" || key == "resolution")
    {
        const std::string l = Lower(value);
        if (l == "reference" || l == "ref") { v.res = Resolution::Reference; return true; }
        if (l == "match_main" || l == "main") { v.res = Resolution::MatchMain; return true; }
        std::uint32_t w = 0, h = 0;
        if (!ParseSize(value, w, h) || w < kMinDesign || h < kMinDesign || w > kMaxDesign || h > kMaxDesign)
        {
            why = "res: '" + value + "' is not reference, match_main or WxH (16..8192)";
            return false;
        }
        v.res  = Resolution::Custom;
        v.resW = w;
        v.resH = h;
        return true;
    }
    if (key == "zoom")
    {
        if (Lower(value) == "fit") { v.zoom = 0.f; return true; }
        float z = 0.f;
        if (!ParseFloat(value, z) || z < kMinZoom || z > kMaxZoom)
        {
            why = "zoom: '" + value + "' is not fit or a number in [1/64, 32]";
            return false;
        }
        // From a fitted view, keep the frame centred at the new zoom (a pan key
        // after this one places it exactly).
        if (v.zoom <= 0.f)
        {
            const float cw = m_pinW ? static_cast<float>(m_pinW) : (m_canvasW > 0.f ? m_canvasW : float(kDefaultBoxW));
            const float ch = m_pinH ? static_cast<float>(m_pinH) : (m_canvasH > 0.f ? m_canvasH : float(kDefaultBoxH));
            v.pan[0] = std::floor((cw - t->designW * z) * 0.5f);
            v.pan[1] = std::floor((ch - t->designH * z) * 0.5f);
        }
        v.zoom = z;
        return true;
    }
    if (key == "pan")
    {
        const std::size_t p = value.find(':');
        float x = 0.f, y = 0.f;
        if (p == std::string::npos || !ParseFloat(value.substr(0, p), x) || !ParseFloat(value.substr(p + 1), y))
        {
            why = "pan: '" + value + "' is not x:y (view pixels)";
            return false;
        }
        if (v.zoom <= 0.f)
        {
            // Panning a fitted view freezes its zoom.
            float z = 1.f, pan[2];
            EffectiveView(*t, t->designW, t->designH, z, pan);
            v.zoom = z;
        }
        v.pan[0] = x;
        v.pan[1] = y;
        return true;
    }
    if (key == "bg" || key == "background")
    {
        const std::string l = Lower(value);
        if (l == "checker") { v.background = UIView::Background::Checker; return true; }
        if (l == "none")    { v.background = UIView::Background::None;    return true; }
        if (l == "solid")   { v.background = UIView::Background::Solid;   return true; }
        if (l.rfind("solid:", 0) == 0)
        {
            const std::vector<std::string> c = SplitList(l.substr(6), ',');
            std::uint32_t r = 0, g = 0, b = 0;
            if (c.size() == 3 && ParseUInt(c[0], r) && ParseUInt(c[1], g) && ParseUInt(c[2], b) && r < 256 && g < 256 &&
                b < 256)
            {
                v.background = UIView::Background::Solid;
                v.solidRgba  = 0xFF000000u | (b << 16) | (g << 8) | r;
                return true;
            }
        }
        why = "bg: '" + value + "' is not checker, none, solid or solid:r,g,b (0..255)";
        return false;
    }
    if (key == "state")
    {
        if (!ParseState(value, v.state))
        {
            why = "state: '" + value + "' is not live, normal, hovered, pressed or disabled";
            return false;
        }
        return true;
    }
    if (key == "statetarget")
    {
        // The state preview on every interactable, or on the selection only.
        const std::string l = Lower(value);
        if (l == "all")      { v.stateSelected = false; return true; }
        if (l == "selected") { v.stateSelected = true;  return true; }
        why = "stateTarget: '" + value + "' is not all or selected";
        return false;
    }
    if (key == "hidden" || key == "showhidden")
    {
        if (!ParseBool(value, v.showHidden)) { why = key + ": '" + value + "' is not a boolean"; return false; }
        return true;
    }
    if (key == "realtime")
    {
        if (!ParseBool(value, v.realtime)) { why = "realtime: '" + value + "' is not a boolean"; return false; }
        return true;
    }
    if (key == "safe")
    {
        std::uint32_t pc = 0;
        if (!ParseUInt(value, pc) || (pc != 0 && pc != 90 && pc != 95))
        {
            why = "safe: '" + value + "' is not 0, 90 or 95";
            return false;
        }
        v.safeArea = static_cast<int>(pc);
        return true;
    }
    if (key == "grid" || key == "rulers" || key == "snap")
    {
        bool b = false;
        if (!ParseBool(value, b)) { why = key + ": '" + value + "' is not a boolean"; return false; }
        (key == "grid" ? v.grid : key == "rulers" ? v.rulers : v.snap) = b;
        return true;
    }
    if (key == "gridref")
    {
        std::uint32_t g = 0;
        if (!ParseUInt(value, g) || g < 1 || g > 256) { why = "gridref: '" + value + "' is not 1..256"; return false; }
        v.gridRef = static_cast<int>(g);
        return true;
    }
    if (key == "frame")
    {
        // A UI-node subject's preview frame: fill, or W x H reference px centred.
        if (Lower(value) == "fill")
        {
            v.frame.mode = UIView::RootFrame::Mode::Fill;
            return true;
        }
        std::uint32_t w = 0, h = 0;
        if (!ParseSize(value, w, h) || w < 1 || h < 1 || w > 16384 || h > 16384)
        {
            why = "frame: '" + value + "' is not fill or WxH (1..16384)";
            return false;
        }
        v.frame.mode = UIView::RootFrame::Mode::Custom;
        v.frame.size = UI::Vec2{ static_cast<float>(w), static_cast<float>(h) };
        return true;
    }
    why = "unknown key '" + keyIn + "' (res, zoom, pan, bg, state, hidden, realtime, size, safe, grid, gridref, "
          "rulers, snap, frame, stateTarget)";
    return false;
}

bool UIEditorWindow::ApplySpec(const std::string& spec, std::string& why)
{
    // All or nothing: the active tab's view and the pin, restored on a refusal.
    Tab* t = ActiveTabPtr();
    const ViewState savedView = t ? t->view : ViewState{};
    const std::uint32_t savedW = m_pinW, savedH = m_pinH;
    for (const std::string& kv : SplitList(spec, ';'))
    {
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            why = "'" + kv + "' is not key=value";
        }
        else if (Set(kv.substr(0, eq), kv.substr(eq + 1), why))
            continue;
        if (t) t->view = savedView;
        m_pinW = savedW;
        m_pinH = savedH;
        return false;
    }
    return true;
}

// ==== Game half ====
void UIEditorWindow::ProcessPendingActions(const UIEditorContext& ctx)
{
    m_frame = ctx.editorFrame;
    UIViewService* views = ctx.renderer ? ctx.renderer->GetUIViews() : nullptr;
    if (views) m_mainScreen = views->MainScreen();
    TickHarness(ctx);

    // Queued opens / closes (UI handlers).
    for (const Entity e : m_pendingOpen)
    {
        std::string why;
        if (!OpenNow(ctx, e, why)) LOG_WARNING("[UIEDITOR] cannot open entity %u: %s", static_cast<unsigned>(e), why.c_str());
    }
    m_pendingOpen.clear();
    // The Content Browser's asset opens.
    for (const std::string& p : m_pendingOpenAssets)
    {
        std::string why;
        if (!OpenAssetNow(ctx, p, why)) LOG_WARNING("[UIEDITOR] cannot open %s: %s", p.c_str(), why.c_str());
    }
    m_pendingOpenAssets.clear();
    // By uid: a script may have closed or opened a tab since Draw queued the close,
    // and an index would then name another tab (closed with discard).
    for (const std::uint32_t uid : m_pendingClose)
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
            if (m_tabs[i].uid == uid)
            {
                CloseTab(ctx, i);
                break;
            }
    m_pendingClose.clear();

    const bool drewLast = m_visibleThisFrame;
    m_visibleThisFrame = false;

    // Stages of tabs closed during Play: their parked ids go back to the free list once
    // the edit world is back (before the closed-window return: closing the whole window
    // in Play closes its tabs too).
    if (!ctx.playing && ctx.world && !m_parkedDiscards.empty())
    {
        for (const auto& snap : m_parkedDiscards)
            for (const auto& n : snap->nodes) ctx.world->UnreserveId(n.id);
        m_parkedDiscards.clear();
    }
    // ... and their rings (a Delete step, an undone Create: ids parked in the edit
    // world - OnDiscard hands them back).
    if (!ctx.playing && !m_parkedSinks.empty())
    {
        for (const auto& s : m_parkedSinks)
            if (s) s->history.Clear();
        m_parkedSinks.clear();
    }

    // Unsaved UI assets are never thrown away unasked. Closed from the Window menu,
    // the command palette or a folded window's X, the window opens again and asks
    // (Save all / Discard / Cancel); only that question's Discard consents.
    if (!m_open && ctx.world && !m_closeConsented && !UnsavedAssets().empty())
    {
        m_open            = true;
        m_confirmCloseAll = true;
        m_focusWindow     = true;
        m_unfold          = true;
        LOG_INFO("[UIEDITOR] the window was closed with unsaved UI assets: asking first");
    }

    // A closed window keeps nothing.
    if (!m_open || !ctx.world)
    {
        m_closeConsented = false;
        m_drag  = DragState{};
        m_nudge = Nudge{};
        if (!m_tabs.empty() && ctx.world) CloseAll(ctx);
        ReleaseView(ctx);
        for (const PendingShot& s : m_shots) Harness::Jobs::Fail(s.job, "closed", "the UI Editor is closed", {});
        m_shots.clear();
        m_pendingSave = m_pendingUndo = 0;
        m_pendingDuplicate = false;
        return;
    }

    // Every tab still names its canvas (a new world: found again by GUID); an asset /
    // template tab's stage is still there (else found by GUID, else spawned again).
    for (int i = static_cast<int>(m_tabs.size()) - 1; i >= 0; --i)
    {
        std::string why;
        Tab& tab = m_tabs[i];
        if (tab.kind == TabKind::Scene ? Revalidate(ctx, tab, why) : RevalidateStage(ctx, tab, why))
        {
            DesignSize(ctx, tab, tab.designW, tab.designH);
            continue;
        }
        LOG_INFO("[UIEDITOR] closed tab '%s': %s", tab.name.c_str(), why.c_str());
        CloseTab(ctx, i);
    }

    // What reached a stage without a sink (Engine.SetUIText, a script) is seen by
    // a periodic capture; the files are looked at as often.
    if (m_frame % kDiskPollFrames == 0)
        for (Tab& tab : m_tabs)
        {
            if (tab.kind == TabKind::Scene || tab.parked || tab.subject == NullEntity) continue;
            Retag(*ctx.world, tab);
            if (tab.kind == TabKind::Asset) OnStageChanged(tab.uid, ctx.world);
            PollDisk(ctx, tab);
        }

    // The shortcuts' requests (EditorLayer::HandleUndoShortcuts), on the active tab.
    if (m_pendingSave)
    {
        std::string why;
        const Tab* a = ActiveTabPtr();
        // In Play the shortcut does what the (disabled) buttons do: nothing. (The
        // editor's exit question still saves a parked stage's last capture.)
        const bool ok = ctx.playing ? (why = "Play mode: save after Stop (the stage is set aside)", false)
                        : (a && a->kind == TabKind::Template && m_pendingSave == 2)
                            ? Convert(ctx, m_pendingSaveAs, true, why)
                            : Save(ctx, m_pendingSave == 2 ? m_pendingSaveAs : std::string(), why);
        if (!ok) LOG_WARNING("[UIEDITOR] save: %s", why.c_str());
        m_pendingSave = 0;
        m_pendingSaveAs.clear();
    }
    while (m_pendingUndo != 0)
    {
        std::string why;
        const bool redo = m_pendingUndo > 0;
        m_pendingUndo += redo ? -1 : 1;
        if (!Undo(ctx, redo, why))
        {
            m_pendingUndo = 0;
            break;
        }
    }
    if (m_pendingDuplicate)
    {
        m_pendingDuplicate = false;
        std::string why;
        if (const Entity s = Sel(ctx); s != NullEntity && Duplicate(ctx, s, why) == NullEntity)
            LOG_WARNING("[UIEDITOR] duplicate: %s", why.c_str());
    }
    if (m_pendingRevert)
    {
        m_pendingRevert = false;
        std::string why;
        if (!Revert(ctx, why)) LOG_WARNING("[UIEDITOR] revert: %s", why.c_str());
    }

    Tab* t = ActiveTabPtr();
    if (t && views && t->parked)
    {
        // Set aside for Play: the view keeps its last picture (frozen: nothing laid
        // out, painted or recorded) until the stage is back.
        if (m_viewId != ~0u && m_viewTab == m_active && m_hasLastDesc)
        {
            UIView::Desc frozen = m_lastDesc;
            frozen.freeze = true;
            views->Request(m_viewId, frozen);
        }
        else
            ReleaseView(ctx);
        m_rows.clear();
        m_rowsTab = -1;
        for (const PendingShot& s : m_shots)
            Harness::Jobs::Fail(s.job, "refused", "the tab's stage is set aside during Play", {});
        m_shots.clear();
        return;
    }
    if (!t || !views || t->subject == NullEntity)
    {
        ReleaseView(ctx);
        for (const PendingShot& s : m_shots) Harness::Jobs::Fail(s.job, "refused", "no canvas open", {});
        m_shots.clear();
        return;
    }

    // A laid-out UI that did not draw the panel last frame (a folded window, a hidden
    // dock tab, F11) hands the view back here, never in Draw. A run with no laid-out UI
    // at all (a hidden editor) or a scripted consumer (the harness, a pinned box, a shot
    // waiting) keeps it: they need the picture unseen.
    const ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
    const bool uiLaidOut = io && io->DisplaySize.x > 0.f && io->DisplaySize.y > 0.f;
    const bool scripted  = !m_harnessOpen.empty() || m_pinW != 0 || !m_shots.empty();
    if (!drewLast && uiLaidOut && !scripted)
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
        m_lastDescHash = 0;
        m_shown = m_complete = false;
    }
    if (m_viewId == ~0u) return;

    UIView::Desc d;
    if (BuildDesc(ctx, *t, d))
    {
        const std::uint64_t h = HashDesc(d);
        if (h != m_lastDescHash) { m_lastDescHash = h; ++m_descSerial; }
        d.serial = m_descSerial;
        views->Request(m_viewId, d);
        m_lastDesc    = d;   // what a parked stage's view keeps showing
        m_hasLastDesc = true;
    }
    UpdateSettle(views->GetStatus(m_viewId));

    // The rows Draw's overlay and hit test read: this frame's layout of what the view
    // paints. A drag whose node died, or that Play caught, ends unrecorded.
    RebuildRows(ctx);
    RememberRes(*t);
    if (m_drag.active && (!ctx.world->IsAlive(m_drag.e) || Frozen(ctx))) m_drag = DragState{};

    // Scripted shots (Editor.UIEditorShot): written once the picture settles.
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
            const UIView::Status vs = views->GetStatus(m_viewId);
            std::string p = s.path;
            for (char& c : p) if (c == '\\') c = '/';
            char hash[20];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(vs.designHash));
            Harness::Jobs::Succeed(s.job, "{\"file\":\"" + p + "\",\"w\":" + std::to_string(vs.width) + ",\"h\":" +
                                          std::to_string(vs.height) + ",\"renders\":" + std::to_string(vs.renders) +
                                          ",\"designHash\":\"" + hash + "\",\"pendingImages\":" +
                                          std::to_string(vs.pendingImages) + "}");
        }
        m_shots.erase(m_shots.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ==== UI ====
void UIEditorWindow::DrawAssetBar(const UIEditorContext& ctx, Tab& t)
{
    const bool unsaved = t.kind == TabKind::Asset && t.stagedText != t.savedText;
    ImGui::BeginDisabled(ctx.playing);
    if (t.kind == TabKind::Asset)
    {
        ImGui::BeginDisabled(!unsaved || !t.readOnlyWhy.empty());
        if (ImGui::Button("Save")) m_pendingSave = 1;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", t.readOnlyWhy.empty() ? "Write the asset (Ctrl+S while the UI Editor has focus)."
                                                          : t.readOnlyWhy.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Save As...")) RequestSave(true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write it as another .iui; this tab follows the new file (Ctrl+Shift+S).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!unsaved && !t.diskChanged);
        if (ImGui::Button("Revert")) m_pendingRevert = true;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Throw the unsaved edits away: the file again (its undo history too).");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "READ-ONLY");
        ImGui::SameLine();
        if (ImGui::Button("Convert to UI Asset...")) RequestSave(true);   // a template's Save As is its Convert
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Write this .ipfb UI template as a .iui UI asset (the editable file) and open it.\n"
                              "The .ipfb stays as it is.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.f, 16.f);
    ImGui::TextDisabled("%s%s", t.path.c_str(), unsaved ? "  (unsaved)" : "");
    if (t.diskChanged)
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "This file changed on disk under your unsaved edits.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) m_pendingRevert = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take the file; your edits are thrown away.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep mine")) t.diskChanged = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep editing; Save writes your version over the file.");
    }
    else if (t.kind == TabKind::Asset && !t.readOnlyWhy.empty())
        ImGui::TextDisabled("%s", t.readOnlyWhy.c_str());
}

void UIEditorWindow::DrawConfirmClose(const UIEditorContext& ctx)
{
    constexpr const char* kOne = "Unsaved UI asset###uieclose";
    constexpr const char* kAll = "Unsaved UI assets###uieclosall";
    if (m_confirmClose != 0)
    {
        Tab* t = TabByUid(m_confirmClose);
        if (!t)
        {
            m_confirmClose = 0;
            return;
        }
        if (!ImGui::IsPopupOpen(kOne)) ImGui::OpenPopup(kOne);
        if (!ImGui::BeginPopupModal(kOne, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::Text("%s has unsaved edits.", t->path.c_str());
        ImGui::TextDisabled("Closing the tab throws them away unless you save.");
        ImGui::Separator();
        const std::uint32_t uid = t->uid;
        auto closeIt = [this, uid] { m_pendingClose.push_back(uid); };
        bool done = false;
        if (ImGui::Button("Save", ImVec2(110.f, 0.f)))
        {
            std::string why;
            if (SaveTab(ctx, *t, std::string(), why, nullptr, nullptr)) closeIt();
            else LOG_WARNING("[UIEDITOR] save: %s", why.c_str());
            done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110.f, 0.f)))
        {
            closeIt();
            done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.f, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = true;
        if (done)
        {
            m_confirmClose = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    if (!m_confirmCloseAll) return;
    if (!ImGui::IsPopupOpen(kAll)) ImGui::OpenPopup(kAll);
    if (!ImGui::BeginPopupModal(kAll, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("These UI assets have unsaved edits:");
    for (const std::string& p : UnsavedAssets()) ImGui::BulletText("%s", p.c_str());
    ImGui::Separator();
    bool done = false;
    if (ImGui::Button("Save All", ImVec2(110.f, 0.f)))
    {
        std::string why;
        if (SaveAllUnsaved(ctx, why)) m_open = false;   // the PPA closes the tabs
        else LOG_WARNING("[UIEDITOR] save: %s", why.c_str());
        done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(110.f, 0.f)))
    {
        m_open           = false;
        m_closeConsented = true;   // the PPA closes the tabs, unsaved edits and all
        done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.f, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = true;
    if (done)
    {
        m_confirmCloseAll = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void UIEditorWindow::DrawToolbar(const UIEditorContext& ctx, Tab& t)
{
    ViewState& v = t.view;

    // A scene tab in Play shows the LIVE played world, read-only.
    if (ctx.playing && t.kind == TabKind::Scene)
    {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "PLAYING");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The played world's canvas, live: what Lua changes shows here. Read-only until Stop;\n"
                              "zoom, pan and the resolution still work.");
        ImGui::SameLine(0.f, 12.f);
    }

    // Resolution.
    char cur[96];
    if (v.res == Resolution::Reference)
        std::snprintf(cur, sizeof(cur), "Reference (%g x %g)", t.designW, t.designH);
    else if (v.res == Resolution::MatchMain)
        std::snprintf(cur, sizeof(cur), "Main viewport (%g x %g)", t.designW, t.designH);
    else
        std::snprintf(cur, sizeof(cur), "%u x %u", v.resW, v.resH);
    ImGui::SetNextItemWidth(210.f);
    if (ImGui::BeginCombo("##uieres", cur))
    {
        if (ImGui::Selectable("Reference resolution", v.res == Resolution::Reference)) v.res = Resolution::Reference;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The canvas's referenceResolution - the screen it was authored for.");
        if (ImGui::Selectable("Match main viewport", v.res == Resolution::MatchMain)) v.res = Resolution::MatchMain;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The game viewport's size, exactly as the game's UI was laid out for it.");
        ImGui::Separator();
        for (const Preset& p : kPresets)
        {
            const bool sel = v.res == Resolution::Custom && v.resW == p.w && v.resH == p.h;
            if (ImGui::Selectable(p.label, sel)) { v.res = Resolution::Custom; v.resW = p.w; v.resH = p.h; }
        }
        ImGui::EndCombo();
    }
    if (v.res == Resolution::Custom)
    {
        ImGui::SameLine();
        int wh[2] = { static_cast<int>(v.resW), static_cast<int>(v.resH) };
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::InputInt2("##uiecustom", wh, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            v.resW = static_cast<std::uint32_t>(std::clamp(wh[0], int(kMinDesign), int(kMaxDesign)));
            v.resH = static_cast<std::uint32_t>(std::clamp(wh[1], int(kMinDesign), int(kMaxDesign)));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rotate")) std::swap(v.resW, v.resH);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap width and height (portrait / landscape).");
    }

    // Zoom.
    ImGui::SameLine(0.f, 16.f);
    if (ImGui::Button("Fit")) v.zoom = 0.f;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit the whole frame in the view (Home).");
    ImGui::SameLine();
    if (ImGui::Button("1:1"))
    {
        std::string why;
        Set("zoom", "1", why);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One design pixel per screen pixel (1).");
    ImGui::SameLine();
    float z = 1.f, pan[2];
    EffectiveView(t, t.designW, t.designH, z, pan);
    ImGui::Text("%.0f%%", z * 100.f);

    // Background.
    ImGui::SameLine(0.f, 16.f);
    ImGui::SetNextItemWidth(90.f);
    int bg = static_cast<int>(v.background);
    if (ImGui::Combo("##uiebg", &bg, "Checker\0Solid\0None\0"))
        v.background = static_cast<UIView::Background>(bg);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the design frame shows behind the UI.");
    if (v.background == UIView::Background::Solid)
    {
        ImGui::SameLine();
        float c[3] = { float(v.solidRgba & 0xFFu) / 255.f, float((v.solidRgba >> 8) & 0xFFu) / 255.f,
                       float((v.solidRgba >> 16) & 0xFFu) / 255.f };
        if (ImGui::ColorEdit3("##uiesolid", c, ImGuiColorEditFlags_NoInputs))
        {
            auto b = [](float f) { return static_cast<std::uint32_t>(std::lround(std::clamp(f, 0.f, 1.f) * 255.f)); };
            v.solidRgba = 0xFF000000u | (b(c[2]) << 16) | (b(c[1]) << 8) | b(c[0]);
        }
    }

    // State preview.
    ImGui::SameLine(0.f, 16.f);
    ImGui::SetNextItemWidth(90.f);
    int st = static_cast<int>(v.state);
    if (ImGui::Combo("##uiestate", &st, "Live\0Normal\0Hovered\0Pressed\0Disabled\0"))
        v.state = static_cast<UI::StatePreview>(st);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every button in one state (the tint transition). Live = as authored.\n"
                          "Lua-drawn focus effects appear only in Play.");
    // One widget's state, the rest live.
    if (v.state != UI::StatePreview::Live)
    {
        ImGui::SameLine();
        ImGui::Checkbox("Selected only", &v.stateSelected);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview the state on the selected node only; the rest stays live.");
    }

    // Second row: the overlays and the editing aids.
    ImGui::Checkbox("Show hidden", &v.showHidden);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lay out visible=false nodes too and draw them faded (35%).");
    ImGui::SameLine();
    ImGui::Checkbox("Realtime", &v.realtime);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Advance this view's UI clock (text jitter, custom text shaders).\nOff: a still picture, no GPU work while idle.");
    ImGui::SameLine(0.f, 16.f);
    ImGui::Checkbox("Snap", &v.snap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap dragged edges to the parent's and the siblings' edges and centres (and the grid when shown).\nHold Alt to drag freely.");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &v.grid);
    if (v.grid)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        if (ImGui::InputInt("##uiegrid", &v.gridRef, 0, 0)) v.gridRef = std::clamp(v.gridRef, 1, 256);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid spacing in reference pixels.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Rulers", &v.rulers);
    ImGui::SameLine(0.f, 16.f);
    {
        // The anchor presets for the selection.
        const Row* sel = FindRow(Sel(ctx));
        const bool can = sel && sel->isRect && !sel->isRoot && !Frozen(ctx);
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Anchors")) ImGui::OpenPopup("##uieanchors");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(can ? "Anchor presets for the selection - the rect stays where it is.\n"
                                    "Alt: the pivot too. Shift: the position too (onto the anchors)."
                                  : "Select a UI node first.");
        DrawAnchorPresetPopup(ctx);
    }
    ImGui::SameLine(0.f, 16.f);
    ImGui::SetNextItemWidth(90.f);
    int safe = v.safeArea == 90 ? 1 : (v.safeArea == 95 ? 2 : 0);
    if (ImGui::Combo("##uiesafe", &safe, "Safe: off\0Safe 90%\0Safe 95%\0"))
        v.safeArea = safe == 1 ? 90 : (safe == 2 ? 95 : 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The title-safe area: keep text and buttons inside it for TVs.");
    ImGui::SameLine(0.f, 16.f);
    ImGui::Checkbox("Panels", &m_panels);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The Palette, Hierarchy and Details beside the designer.");

    // A UI-node subject (a row template, a subtree no canvas owns) previews in a
    // frame: the whole design screen, or a W x H box of its own, centred.
    if (ctx.world && t.subject != NullEntity && !ctx.world->HasComponent<UI::UICanvas>(t.subject))
    {
        ImGui::SameLine(0.f, 16.f);
        bool custom = v.frame.mode == UIView::RootFrame::Mode::Custom;
        if (ImGui::Checkbox("Frame", &custom))
            v.frame.mode = custom ? UIView::RootFrame::Mode::Custom : UIView::RootFrame::Mode::Fill;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: the root's parent is the whole design screen.\n"
                              "On: the root is this size (reference px), centred - a row template's own size.");
        if (custom)
        {
            ImGui::SameLine();
            int wh[2] = { static_cast<int>(v.frame.size.x), static_cast<int>(v.frame.size.y) };
            ImGui::SetNextItemWidth(110.f);
            if (ImGui::InputInt2("##uieframe", wh, ImGuiInputTextFlags_EnterReturnsTrue))
                v.frame.size = UI::Vec2{ static_cast<float>(std::clamp(wh[0], 1, 16384)),
                                         static_cast<float>(std::clamp(wh[1], 1, 16384)) };
        }
    }
}

void UIEditorWindow::DrawOverlay(const UIEditorContext& ctx, Tab& t, float ox, float oy, float z, const float pan[2],
                                 float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto V = [&](float x, float y) { return ImVec2(ox + pan[0] + x * z, oy + pan[1] + y * z); };
    const float dw = t.designW, dh = t.designH;
    dl->PushClipRect(ImVec2(ox, oy), ImVec2(ox + width, oy + height), true);

    // The grid, every gridRef REFERENCE px - only while its lines stay apart.
    if (t.view.grid)
    {
        const float g = static_cast<float>((std::max)(1, t.view.gridRef)) * m_rowsScale;
        if (g * z >= 6.f)
        {
            const ImU32 c = IM_COL32(255, 255, 255, 24);
            for (float x = 0.f; x <= dw + 0.5f; x += g) dl->AddLine(V(x, 0.f), V(x, dh), c);
            for (float y = 0.f; y <= dh + 0.5f; y += g) dl->AddLine(V(0.f, y), V(dw, y), c);
        }
    }

    // The safe area.
    if (t.view.safeArea > 0)
    {
        const float f = static_cast<float>(100 - t.view.safeArea) / 200.f;
        dl->AddRect(V(dw * f, dh * f), V(dw * (1.f - f), dh * (1.f - f)), IM_COL32(255, 210, 80, 150));
        char s[24];
        std::snprintf(s, sizeof(s), "Safe %d%%", t.view.safeArea);
        dl->AddText(ImVec2(V(dw * f, dh * f).x + 4.f, V(dw * f, dh * f).y + 2.f), IM_COL32(255, 210, 80, 190), s);
    }

    // Hover.
    if (m_hoverEntity != NullEntity && m_hoverEntity != Sel(ctx) && !m_drag.active)
        if (const Row* r = FindRow(m_hoverEntity); r && !r->isRoot)
            dl->AddRect(V(r->rect.mn.x, r->rect.mn.y), V(r->rect.mx.x, r->rect.mx.y), IM_COL32(120, 200, 255, 140));

    // The selection: its parent's frame, its rect, the handles, the pivot.
    if (const Row* s = FindRow(Sel(ctx)))
    {
        if (s->isRoot)
            dl->AddRect(V(0.f, 0.f), V(dw, dh), IM_COL32(200, 120, 255, 220), 0.f, 0, 2.f);
        else
        {
            UI::Rect rr = s->rect;
            UI::Vec2 pivot = s->isRect ? s->authored.pivot : UI::Vec2{ 0.5f, 0.5f };
            // Mid-drag the rows are last frame's: resolve what the drag just wrote.
            if (m_drag.active && m_drag.e == s->e && ctx.world)
                if (const UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(s->e))
                {
                    rr    = UIRectEdit::Resolve(*ur, m_drag.rctx);
                    pivot = ur->pivot;
                }
            dl->AddRect(V(s->parentRect.mn.x, s->parentRect.mn.y), V(s->parentRect.mx.x, s->parentRect.mx.y),
                        IM_COL32(150, 150, 160, 90));
            // The anchors: four triangles pointing into the anchor box, one
            // flower where a point anchor puts them on one spot.
            if (s->isRect && ctx.world)
            {
                const UI::UIRect* ua = ctx.world->GetComponent<UI::UIRect>(s->e);
                const UI::UIRect& ar = ua ? *ua : s->authored;
                const UI::Rect& p = s->parentRect;
                const float pw = p.mx.x - p.mn.x, ph = p.mx.y - p.mn.y;
                const float ax[2] = { p.mn.x + std::clamp(ar.anchorMin.x, 0.f, 1.f) * pw, p.mn.x + std::clamp(ar.anchorMax.x, 0.f, 1.f) * pw };
                const float ay[2] = { p.mn.y + std::clamp(ar.anchorMin.y, 0.f, 1.f) * ph, p.mn.y + std::clamp(ar.anchorMax.y, 0.f, 1.f) * ph };
                const ImU32 ac = IM_COL32(255, 255, 255, 230);
                const float k = 9.f;
                auto tri = [&](float x, float y, float sx, float sy)
                {
                    const ImVec2 c = V(x, y);
                    dl->AddTriangleFilled(c, ImVec2(c.x - sx * k, c.y - sy * k * 0.45f), ImVec2(c.x - sx * k * 0.45f, c.y - sy * k), ac);
                };
                tri(ax[0], ay[0],  1.f,  1.f);   // TL: points down-right into the box
                tri(ax[1], ay[0], -1.f,  1.f);   // TR
                tri(ax[1], ay[1], -1.f, -1.f);   // BR
                tri(ax[0], ay[1],  1.f, -1.f);   // BL
            }
            const ImVec2 a = V(rr.mn.x, rr.mn.y), b = V(rr.mx.x, rr.mx.y);
            dl->AddRect(a, b, IM_COL32(90, 170, 255, 235), 0.f, 0, 1.5f);
            if (s->isRect && !Frozen(ctx))
                for (int i = 0; i < 8; ++i)
                {
                    const UI::Vec2 hp = UIRectEdit::HandlePoint(rr, i);
                    const ImVec2 h = V(hp.x, hp.y);
                    dl->AddRectFilled(ImVec2(h.x - kHandleViewPx, h.y - kHandleViewPx),
                                      ImVec2(h.x + kHandleViewPx, h.y + kHandleViewPx), IM_COL32(90, 170, 255, 255), 1.f);
                }
            // A selected textured image's 9-slice borders - the lines the paint
            // cuts it along - each with a diamond to drag.
            if (s->isRect && !Frozen(ctx) && ctx.world)
                if (const UI::UIImage* img = ctx.world->GetComponent<UI::UIImage>(s->e); img && !img->texturePath.empty())
                {
                    float b[4];
                    EffectiveBorders(*img, rr, b);
                    const ImU32 sc = IM_COL32(255, 150, 60, 190);
                    dl->AddLine(V(rr.mn.x + b[0], rr.mn.y), V(rr.mn.x + b[0], rr.mx.y), sc);
                    dl->AddLine(V(rr.mx.x - b[2], rr.mn.y), V(rr.mx.x - b[2], rr.mx.y), sc);
                    dl->AddLine(V(rr.mn.x, rr.mn.y + b[1]), V(rr.mx.x, rr.mn.y + b[1]), sc);
                    dl->AddLine(V(rr.mn.x, rr.mx.y - b[3]), V(rr.mx.x, rr.mx.y - b[3]), sc);
                    const float k = kHandleViewPx + 1.f;
                    for (int i = 0; i < 4; ++i)
                    {
                        const UI::Vec2 hp = SliceHandlePoint(rr, b, i);
                        const ImVec2 c = V(hp.x, hp.y);
                        dl->AddQuadFilled(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x, c.y + k),
                                          ImVec2(c.x - k, c.y), IM_COL32(255, 150, 60, 255));
                    }
                }
            const float px = rr.mn.x + (rr.mx.x - rr.mn.x) * std::clamp(pivot.x, 0.f, 1.f);
            const float py = rr.mn.y + (rr.mx.y - rr.mn.y) * std::clamp(pivot.y, 0.f, 1.f);
            const ImVec2 pv = V(px, py);
            dl->AddCircle(pv, 4.f, IM_COL32(255, 200, 60, 255), 12, 1.5f);
            if (m_drag.active && m_drag.e == s->e && ctx.world)
                if (const UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(s->e))
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%.0f x %.0f  @ %.0f, %.0f", ur->size.x, ur->size.y,
                                  ur->offset.x, ur->offset.y);
                    dl->AddText(ImVec2(a.x, a.y - 18.f), IM_COL32(255, 255, 255, 235), buf);
                }
        }
    }

    // Snap guides.
    if (m_drag.active)
    {
        const ImU32 c = IM_COL32(255, 90, 200, 220);
        if (m_drag.hasGuideX) dl->AddLine(V(m_drag.guideX, 0.f), V(m_drag.guideX, dh), c);
        if (m_drag.hasGuideY) dl->AddLine(V(0.f, m_drag.guideY), V(dw, m_drag.guideY), c);
    }

    // Rulers along the top and the left edge, in design pixels.
    if (t.view.rulers)
    {
        const float band = 16.f;
        const ImU32 bgc = IM_COL32(18, 18, 20, 220), tick = IM_COL32(170, 170, 180, 200);
        dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + width, oy + band), bgc);
        dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + band, oy + height), bgc);
        static const float kSteps[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000 };
        float major = kSteps[0];
        for (const float s : kSteps) { major = s; if (s * z >= 60.f) break; }
        const float minor = major / 5.f;
        char lab[16];
        const float x0 = (ox + band - (ox + pan[0])) / z, x1 = (ox + width - (ox + pan[0])) / z;
        for (float x = std::floor(x0 / minor) * minor; x <= x1; x += minor)
        {
            const float sx = ox + pan[0] + x * z;
            const bool  m  = std::fabs(std::fmod(x, major)) < minor * 0.5f;
            dl->AddLine(ImVec2(sx, oy + band - (m ? 8.f : 4.f)), ImVec2(sx, oy + band), tick);
            if (m) { std::snprintf(lab, sizeof(lab), "%.0f", x); dl->AddText(ImVec2(sx + 2.f, oy), tick, lab); }
        }
        const float y0 = (oy + band - (oy + pan[1])) / z, y1 = (oy + height - (oy + pan[1])) / z;
        for (float y = std::floor(y0 / minor) * minor; y <= y1; y += minor)
        {
            const float sy = oy + pan[1] + y * z;
            const bool  m  = std::fabs(std::fmod(y, major)) < minor * 0.5f;
            dl->AddLine(ImVec2(ox + band - (m ? 8.f : 4.f), sy), ImVec2(ox + band, sy), tick);
            if (m) { std::snprintf(lab, sizeof(lab), "%.0f", y); dl->AddText(ImVec2(ox + 1.f, sy + 1.f), tick, lab); }
        }
        // The cursor's position on both rulers.
        if (m_hover)
        {
            const ImU32 cc = IM_COL32(255, 200, 60, 230);
            const float cx = ox + pan[0] + m_hoverX * z, cy = oy + pan[1] + m_hoverY * z;
            dl->AddLine(ImVec2(cx, oy), ImVec2(cx, oy + band), cc);
            dl->AddLine(ImVec2(ox, cy), ImVec2(ox + band, cy), cc);
        }
    }
    dl->PopClipRect();
}

void UIEditorWindow::DrawCanvas(const UIEditorContext& ctx, Tab& t, float width, float height)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(26, 26, 26, 255));

    // The view box follows the canvas: quantised up to 16 px (the target always
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

    ImGui::InvisibleButton("##uiecanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    // The effective zoom / pan (a fitted view recomputes them every frame).
    float z = 1.f, pan[2] = { 0.f, 0.f };
    EffectiveView(t, t.designW, t.designH, z, pan);
    if (hovered && io.MouseWheel != 0.f)
    {
        const float nz = std::clamp(z * std::pow(1.25f, io.MouseWheel), kMinZoom, kMaxZoom);
        const float cx = io.MousePos.x - origin.x, cy = io.MousePos.y - origin.y;
        // Keep the design point under the cursor where it is.
        t.view.pan[0] = cx - (cx - pan[0]) * (nz / z);
        t.view.pan[1] = cy - (cy - pan[1]) * (nz / z);
        t.view.zoom   = nz;
        z = nz;
        pan[0] = t.view.pan[0];
        pan[1] = t.view.pan[1];
    }
    // Drops: a Palette widget or a UI template (.ipfb, from the Palette or the
    // Content Browser) is made where it lands; a texture goes onto the image under it
    // or becomes a new Image there. Run after this function's last row read: making
    // something lays the rows out again.
    std::function<void()> dropped;
    if (!Frozen(ctx) && ImGui::BeginDragDropTarget())
    {
        const UI::Vec2 at{ (io.MousePos.x - origin.x - pan[0]) / z, (io.MousePos.y - origin.y - pan[1]) / z };
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("UIE_WIDGET"))
        {
            int w = 0;
            std::memcpy(&w, p->Data, sizeof(w));
            dropped = [this, &ctx, w, at]
            {
                std::string why;
                if (CreateWidget(ctx, UITemplates::Name(static_cast<UITemplates::Widget>(w)), DropParent(ctx, at), true,
                                 at, why) == NullEntity)
                    LOG_WARNING("[UIEDITOR] drop: %s", why.c_str());
            };
        }
        else if (const ImGuiPayload* q = ImGui::AcceptDragDropPayload("IPFB_PATH"))
        {
            const std::string path(static_cast<const char*>(q->Data));
            dropped = [this, &ctx, path, at]
            {
                std::string why;
                if (CreateWidget(ctx, path, DropParent(ctx, at), true, at, why) == NullEntity)
                    LOG_WARNING("[UIEDITOR] drop %s: %s", path.c_str(), why.c_str());
            };
        }
        else if (const ImGuiPayload* u = ImGui::AcceptDragDropPayload("IUI_PATH"))   // a .iui
        {
            const std::string path(static_cast<const char*>(u->Data));
            dropped = [this, &ctx, path, at]
            {
                std::string why;
                if (CreateWidget(ctx, path, DropParent(ctx, at), true, at, why) == NullEntity)
                    LOG_WARNING("[UIEDITOR] drop %s: %s", path.c_str(), why.c_str());
            };
        }
        else if (const ImGuiPayload* x = ImGui::AcceptDragDropPayload("ITEX_PATH"))
        {
            const std::string path(static_cast<const char*>(x->Data));
            dropped = [this, &ctx, path, at]
            {
                std::string why;
                if (DropTexture(ctx, path, at, why) == NullEntity)
                    LOG_WARNING("[UIEDITOR] drop %s: %s", path.c_str(), why.c_str());
            };
        }
        ImGui::EndDragDropTarget();
    }
    // Middle or right drag pans; so does Space + left drag (left is the selection's).
    if (!m_drag.active && hovered && ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_spacePan = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_spacePan = false;
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f) ||
                   (m_spacePan && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))))
    {
        if (t.view.zoom <= 0.f) t.view.zoom = z;
        t.view.pan[0] = pan[0] + io.MouseDelta.x;
        t.view.pan[1] = pan[1] + io.MouseDelta.y;
        pan[0] = t.view.pan[0];
        pan[1] = t.view.pan[1];
    }

    UIViewService* views = ctx.renderer ? ctx.renderer->GetUIViews() : nullptr;
    const bool own = views && m_viewId != ~0u && m_viewTab == m_active;
    const UIView::Status st = own ? views->GetStatus(m_viewId) : UIView::Status{};
    const std::uint64_t srv = own ? views->Srv(m_viewId) : 0;
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    if (srv && st.hasOutput)
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(srv)), origin,
                     ImVec2(origin.x + static_cast<float>(st.width), origin.y + static_cast<float>(st.height)));
    else
        dl->AddText(ImVec2(origin.x + 24.f, origin.y + 24.f), IM_COL32(200, 200, 200, 255), "Preparing the view...");

    // The design frame (the screen the UI is laid out for) and its size.
    const ImVec2 f0(origin.x + pan[0], origin.y + pan[1]);
    const ImVec2 f1(f0.x + t.designW * z, f0.y + t.designH * z);
    dl->AddRect(ImVec2(f0.x - 1.f, f0.y - 1.f), ImVec2(f1.x + 1.f, f1.y + 1.f), IM_COL32(110, 110, 120, 255));
    if (!t.view.rulers)   // the rulers (and the toolbar) say it already; they would cover it
    {
        char label[64];
        std::snprintf(label, sizeof(label), "%g x %g", t.designW, t.designH);
        dl->AddText(ImVec2(f0.x, f0.y - ImGui::GetTextLineHeight() - 2.f), IM_COL32(150, 150, 160, 255), label);
    }
    if (!st.why.empty())
        dl->AddText(ImVec2(origin.x + 24.f, origin.y + height - ImGui::GetTextLineHeight() - 6.f),
                    IM_COL32(255, 180, 80, 255), st.why.c_str());
    dl->PopClipRect();

    // ---- The cursor, selection and the Rect Tool ----
    m_hover = hovered;
    const float dx = (io.MousePos.x - origin.x - pan[0]) / z, dy = (io.MousePos.y - origin.y - pan[1]) / z;
    if (hovered)
    {
        m_hoverX = dx;
        m_hoverY = dy;
    }
    m_hoverEntity = (hovered && !m_drag.active && !m_spacePan) ? HitTest(ctx, UI::Vec2{ dx, dy }) : NullEntity;

    const Row* sel = FindRow(Sel(ctx));
    // A handle of the selection under the cursor.
    int handle = -1;
    if (hovered && sel && sel->isRect && !sel->isRoot && !Frozen(ctx))
        for (int i = 0; i < 8 && handle < 0; ++i)
        {
            const UI::Vec2 hp = UIRectEdit::HandlePoint(sel->rect, i);
            const float hx = origin.x + pan[0] + hp.x * z, hy = origin.y + pan[1] + hp.y * z;
            if (std::fabs(io.MousePos.x - hx) <= kGrabViewPx && std::fabs(io.MousePos.y - hy) <= kGrabViewPx) handle = i;
        }
    // The pivot marker and the anchor handles (after the rect handles).
    if (handle < 0 && hovered && sel && sel->isRect && !sel->isRoot && !Frozen(ctx) && ctx.world)
        if (const UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(sel->e))
        {
            auto nearPt = [&](float x, float y)
            {
                const float hx = origin.x + pan[0] + x * z, hy = origin.y + pan[1] + y * z;
                return std::fabs(io.MousePos.x - hx) <= kGrabViewPx && std::fabs(io.MousePos.y - hy) <= kGrabViewPx;
            };
            const UI::Rect& rr = sel->rect;
            if (nearPt(rr.mn.x + (rr.mx.x - rr.mn.x) * std::clamp(ur->pivot.x, 0.f, 1.f),
                       rr.mn.y + (rr.mx.y - rr.mn.y) * std::clamp(ur->pivot.y, 0.f, 1.f)))
                handle = kPivotHandle;
            else
            {
                const UI::Rect& p = sel->parentRect;
                const float pw = p.mx.x - p.mn.x, ph = p.mx.y - p.mn.y;
                const float x0 = p.mn.x + std::clamp(ur->anchorMin.x, 0.f, 1.f) * pw, x1 = p.mn.x + std::clamp(ur->anchorMax.x, 0.f, 1.f) * pw;
                const float y0 = p.mn.y + std::clamp(ur->anchorMin.y, 0.f, 1.f) * ph, y1 = p.mn.y + std::clamp(ur->anchorMax.y, 0.f, 1.f) * ph;
                const bool point = std::fabs(x1 - x0) * z < 2.f * kGrabViewPx && std::fabs(y1 - y0) * z < 2.f * kGrabViewPx;
                if (point && nearPt((x0 + x1) * 0.5f, (y0 + y1) * 0.5f)) handle = kAnchorAll;
                else if (nearPt(x0, y0)) handle = kAnchorTL;
                else if (nearPt(x1, y0)) handle = kAnchorTR;
                else if (nearPt(x1, y1)) handle = kAnchorBR;
                else if (nearPt(x0, y1)) handle = kAnchorBL;
            }
        }
    // A selected textured image's 9-slice diamonds (last: the rect's own handles win).
    if (handle < 0 && hovered && sel && sel->isRect && !sel->isRoot && !Frozen(ctx) && ctx.world)
        if (const UI::UIImage* img = ctx.world->GetComponent<UI::UIImage>(sel->e); img && !img->texturePath.empty())
        {
            float b[4];
            EffectiveBorders(*img, sel->rect, b);
            for (int i = 0; i < 4 && handle < 0; ++i)
            {
                const UI::Vec2 hp = SliceHandlePoint(sel->rect, b, i);
                const float hx = origin.x + pan[0] + hp.x * z, hy = origin.y + pan[1] + hp.y * z;
                if (std::fabs(io.MousePos.x - hx) <= kGrabViewPx && std::fabs(io.MousePos.y - hy) <= kGrabViewPx)
                    handle = kSliceL + i;
            }
        }
    // Double-click a text node: edit its text in place (Enter applies, Ctrl+Enter
    // a new line, Esc cancels). Its first click selected it and began an empty drag.
    if (!m_spacePan && hovered && handle < 0 && !Frozen(ctx) && ctx.world &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        const Entity hit = HitTest(ctx, UI::Vec2{ dx, dy });
        if (const Row* r = FindRow(hit); r && !r->isRoot)
            if (const UI::UIText* tx = ctx.world->GetComponent<UI::UIText>(hit))
            {
                if (m_drag.active) EndDrag(ctx);
                if (hit != Sel(ctx)) SelectNode(ctx, hit);
                m_textEdit = hit;
                m_textBuf.assign(tx->text.begin(), tx->text.end());
                m_textBuf.push_back('\0');
                m_textBuf.resize((std::max)(m_textBuf.size(), static_cast<std::size_t>(256)));
                m_textFocus = true;
                m_textAt[0] = origin.x + pan[0] + r->rect.mn.x * z;
                m_textAt[1] = origin.y + pan[1] + r->rect.mx.y * z + 2.f;
            }
    }
    if (!m_drag.active && !m_spacePan && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsKeyDown(ImGuiKey_Space) && m_textEdit == NullEntity)
    {
        std::string why;
        if (handle >= 0)
            BeginDrag(ctx, sel->e, handle, why);
        else
        {
            // Click: select what is under the cursor (Alt: the next one under it);
            // press-and-drag then moves it, as in UMG.
            const Entity hit = HitTest(ctx, UI::Vec2{ dx, dy }, io.KeyAlt, Sel(ctx));
            if (hit != Sel(ctx)) SelectNode(ctx, hit);
            if (const Row* r = FindRow(hit); r && r->isRect && !r->isRoot && !Frozen(ctx))
                BeginDrag(ctx, hit, UIRectEdit::kBody, why);
        }
        if (m_drag.active)
        {
            m_drag.startMouse[0] = io.MousePos.x;
            m_drag.startMouse[1] = io.MousePos.y;
        }
    }
    if (m_drag.active)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const UI::Vec2 d{ (io.MousePos.x - m_drag.startMouse[0]) / z, (io.MousePos.y - m_drag.startMouse[1]) / z };
            UpdateDrag(ctx, d, io.KeyCtrl, io.KeyShift, io.KeyAlt);
        }
        else
            EndDrag(ctx);
    }
    const int cursorHandle = m_drag.active ? m_drag.handle : handle;
    if (cursorHandle >= 0)
    {
        switch (cursorHandle)
        {
        case UIRectEdit::kTL: case UIRectEdit::kBR: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
        case UIRectEdit::kTR: case UIRectEdit::kBL: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
        case UIRectEdit::kT:  case UIRectEdit::kB:  ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);   break;
        case UIRectEdit::kL:  case UIRectEdit::kR:  ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);   break;
        case kSliceL:         case kSliceR:         ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);   break;
        case kSliceT:         case kSliceB:         ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);   break;
        default:                                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);  break;
        }
    }

    DrawOverlay(ctx, t, origin.x, origin.y, z, pan, width, height);
    if (dropped) dropped();
}

void UIEditorWindow::Draw(const UIEditorContext& ctx)
{
    if (!m_open)
    {
        m_focused = false;
        return;
    }
    // Draw holds a Tab* across its panels: anything they run that would add or
    // close a tab is queued for the PPA while this is set (see m_inDraw).
    struct DrawScope
    {
        bool& flag;
        explicit DrawScope(bool& f) : flag(f) { flag = true; }
        ~DrawScope() { flag = false; }
    } drawScope(m_inDraw);
    ImGui::SetNextWindowSize(ImVec2(1280.f, 800.f), ImGuiCond_FirstUseEver);
    // Asked to close with unsaved UI assets while folded: unfold it to ask.
    if (m_unfold)
    {
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        m_unfold = false;
    }
    if (m_focusWindow)
    {
        ImGui::SetNextWindowFocus();
        m_focusWindow = false;
    }
    if (!ImGui::Begin("UI Editor###UIEditor", &m_open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        // Folded (or a hidden dock tab) is not visible: m_visibleThisFrame stays
        // false and the next PPA hands the view back.
        m_focused = false;
        ImGui::End();
        return;
    }
    m_visibleThisFrame = true;
    // The asset tabs' shortcuts (EditorLayer::HandleUndoShortcuts, next frame).
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // The window's X with unsaved UI assets asks first (Save all / Discard / Cancel).
    if (!m_open && !UnsavedAssets().empty())
    {
        m_open            = true;
        m_confirmCloseAll = true;
    }
    DrawConfirmClose(ctx);

    if (m_tabs.empty())
    {
        ImGui::TextDisabled("No canvas open.");
        ImGui::TextDisabled("Select a Canvas (or any UI node) and use \"Edit in UI Editor\" in the Inspector or the "
                            "Outliner's context menu, or double-click a .iui UI asset in the Content Browser.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##uietabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            const Tab& tb = m_tabs[i];
            bool keep = true;
            const bool unsaved = tb.kind == TabKind::Asset && tb.stagedText != tb.savedText;
            // The id is the tab's own (a respawned stage is a new entity, still this tab).
            // UnsavedDocument: the dot, and an X that asks instead of closing.
            const std::string label = tb.name + "###uietab" + std::to_string(tb.uid);
            ImGuiTabItemFlags f = (i == m_focusTab) ? ImGuiTabItemFlags_SetSelected : 0;
            if (unsaved) f |= ImGuiTabItemFlags_UnsavedDocument;
            if (ImGui::BeginTabItem(label.c_str(), &keep, f))
            {
                if ((m_focusTab < 0 || m_focusTab == i) && m_active != i) SwitchTab(ctx, i);
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered())
            {
                if (tb.kind == TabKind::Scene)
                    ImGui::SetTooltip("%s (entity %u)%s%s", tb.name.c_str(), static_cast<unsigned>(tb.subject),
                                      tb.guid.IsValid() ? "\nGUID " : "", HexGuid(tb.guid).c_str());
                else
                    ImGui::SetTooltip("%s\n%s%s%s", tb.path.c_str(),
                                      tb.kind == TabKind::Template ? "UI template (.ipfb), read-only" : "UI asset (.iui)",
                                      unsaved ? " - unsaved edits" : "", tb.diskChanged ? " - changed on disk" : "");
            }
            if (!keep)
            {
                if (unsaved) m_confirmClose = tb.uid;   // ask first
                else         m_pendingClose.push_back(tb.uid);
            }
        }
        m_focusTab = -1;
        ImGui::EndTabBar();
    }
    Tab* t = ActiveTabPtr();
    if (!t) { ImGui::End(); return; }
    // An asset / template tab's file: its path, Save / Save As / Revert / Convert,
    // and what is wrong with it (read-only, changed on disk, set aside for Play).
    if (t->kind != TabKind::Scene)
    {
        DrawAssetBar(ctx, *t);
        // The picture below is the stage's last one (the view is frozen).
        if (t->parked)
            ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f),
                               "PAUSED during Play - the stage is set aside (this is its last picture); it comes back "
                               "at Stop, with its undo and every unsaved edit.");
    }

    // Keys while the window is focused (and nothing types): arrows nudge the
    // selection (Shift: 10 ref px; one undo step while held), Delete, F2 rename,
    // F frames the selection, Home fits, 1 shows 1:1, Ctrl+[ / Ctrl+] reorder.
    // Not while a popup or a modal is up (its own window counts as focused here:
    // Delete would remove the selection behind an Unsaved question).
    const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                      m_renaming == NullEntity &&
                      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    bool arrowHeld = false;
    if (keys)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float step = io.KeyShift ? 10.f : 1.f;
        if (!io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  NudgeSelection(ctx, -step, 0.f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) NudgeSelection(ctx,  step, 0.f);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    NudgeSelection(ctx, 0.f, -step);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  NudgeSelection(ctx, 0.f,  step);
        }
        arrowHeld = ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
                    ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow);
        const Row* sel = FindRow(Sel(ctx));
        if (sel && !sel->isRoot && !Frozen(ctx) && !InstanceNode(ctx, sel->e))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && ctx.deleteEntity) ctx.deleteEntity(sel->e);
            else if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            {
                m_renaming = sel->e;
                const std::string& n = ctx.world ? ctx.world->GetName(sel->e) : std::string();
                std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", n.c_str());
                m_renameFocus = true;
            }
            else if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) ||
                                    ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) && ctx.moveUINode)
            {
                // One place earlier / later among its siblings (paint order).
                int at = 0, i = 0;
                for (const Row& r : m_rows)
                    if (r.parent == sel->parent && !r.isRoot)
                    {
                        if (r.e == sel->e) at = i;
                        ++i;
                    }
                const int to = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) ? at - 1 : at + 1;
                std::string why;
                if (to >= 0 && to < i) ctx.moveUINode(sel->e, sel->parent, to, &why);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameSelection(ctx, *t);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) t->view.zoom = 0.f;
        if (ImGui::IsKeyPressed(ImGuiKey_1, false) && !io.KeyCtrl)
        {
            std::string why;
            Set("zoom", "1", why);
        }
    }
    if (!arrowHeld && m_nudge.active) EndNudge(ctx);

    DrawToolbar(ctx, *t);

    // Palette + Hierarchy | the designer | Details / Check, the side columns
    // resized by the splitters between them (editor_prefs.ini).
    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const float  statusH = ImGui::GetFrameHeightWithSpacing();
    const float  bodyH   = (std::max)(64.f, avail.y - statusH);
    const float  splitW  = 6.f;
    const bool   panels  = m_panels && avail.x > 560.f;
    const float  leftW   = panels ? std::clamp(m_leftW, 150.f, avail.x * 0.35f) : 0.f;
    const float  rightW  = panels ? std::clamp(m_rightW, 220.f, avail.x * 0.4f) : 0.f;
    const float  canvasW = (std::max)(64.f, avail.x - (panels ? leftW + rightW + 2.f * splitW : 0.f));
    const float  canvasH = bodyH;
    auto splitter = [&](const char* id, float& w, float sign)
    {
        ImGui::InvisibleButton(id, ImVec2(splitW, bodyH));
        const bool act = ImGui::IsItemActive();
        if (act || ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (act) w = std::clamp(w + sign * ImGui::GetIO().MouseDelta.x, 150.f, 900.f);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2.f, a.y), ImVec2(b.x - 2.f, b.y),
                                                  act ? IM_COL32(90, 150, 230, 255) : IM_COL32(52, 52, 58, 255));
    };
    if (panels)
    {
        ImGui::BeginChild("##uieleft", ImVec2(leftW, bodyH), ImGuiChildFlags_None);
        DrawPalette(ctx, *t);
        DrawHierarchy(ctx, *t);
        ImGui::EndChild();
        ImGui::SameLine(0.f, 0.f);
        splitter("##uiesplitl", m_leftW, 1.f);
        ImGui::SameLine(0.f, 0.f);
    }
    ImGui::BeginChild("##uiecanvaschild", ImVec2(canvasW, canvasH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas(ctx, *t, canvasW, canvasH);
    ImGui::EndChild();
    if (panels)
    {
        ImGui::SameLine(0.f, 0.f);
        splitter("##uiesplitr", m_rightW, -1.f);
        ImGui::SameLine(0.f, 0.f);
        ImGui::BeginChild("##uieright", ImVec2(rightW, bodyH), ImGuiChildFlags_None);
        if (ImGui::BeginTabBar("##uieside"))
        {
            if (ImGui::BeginTabItem("Details"))
            {
                m_sideTab = 0;
                DrawDetails(ctx, *t);
                ImGui::EndTabItem();
            }
            char check[32];
            std::snprintf(check, sizeof(check), m_issuesRun ? "Check (%zu)###uiecheck" : "Check###uiecheck",
                          m_issues.size());
            if (ImGui::BeginTabItem(check))
            {
                m_sideTab = 1;
                DrawCheck(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
    }
    DrawTextEditor(ctx);

    // F2: rename the selection (one undo step, the Outliner's rename). Opened ONCE
    // (m_renameFocus marks the first frame): reopened every frame, a click outside
    // could never close it and it held the mouse.
    if (m_renaming != NullEntity)
    {
        if (m_renameFocus) ImGui::OpenPopup("##uierename");
        if (ImGui::BeginPopup("##uierename"))
        {
            if (m_renameFocus) { ImGui::SetKeyboardFocusHere(); m_renameFocus = false; }
            const bool enter = ImGui::InputText("##name", m_renameBuf, sizeof(m_renameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (enter)
            {
                if (ctx.rename && m_renameBuf[0] && ctx.world && ctx.world->IsAlive(m_renaming))
                    ctx.rename(m_renaming, m_renameBuf);
                m_renaming = NullEntity;
                ImGui::CloseCurrentPopup();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                m_renaming = NullEntity;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else
            m_renaming = NullEntity;   // clicked away
    }

    // Status line: the selection's numbers, else the subject; the cursor.
    UIViewService* views = ctx.renderer ? ctx.renderer->GetUIViews() : nullptr;
    const bool own = views && m_viewId != ~0u && m_viewTab == m_active;
    const UIView::Status st = own ? views->GetStatus(m_viewId) : UIView::Status{};
    const Row* sel = FindRow(Sel(ctx));
    if (sel && sel->isRect && ctx.world)
    {
        const UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(sel->e);
        const UI::UIRect& r = ur ? *ur : sel->authored;
        ImGui::TextDisabled("%s  |  offset (%.1f, %.1f)  size %.1f x %.1f  anchors (%.2f, %.2f)-(%.2f, %.2f)  pivot (%.2f, %.2f)"
                            "  sibling %d", ctx.world->GetName(sel->e).c_str(), r.offset.x, r.offset.y, r.size.x,
                            r.size.y, r.anchorMin.x, r.anchorMin.y, r.anchorMax.x, r.anchorMax.y, r.pivot.x, r.pivot.y,
                            r.siblingIndex);
    }
    else
        ImGui::TextDisabled("%s  |  %g x %g  scale %.3f  |  %u nodes  %u draws  %u verts%s", t->name.c_str(),
                            t->designW, t->designH, st.canvasScale, st.nodes, st.cmds, st.verts,
                            st.truncated ? "  (TRUNCATED: the 65535-index draw list is full)" : "");
    if (m_hover)
    {
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextDisabled("%.0f, %.0f px", std::floor(m_hoverX), std::floor(m_hoverY));
    }
    ImGui::End();
}

// ==== The layout rows, selection, the Rect Tool ====
namespace
{
    const char* const kRectHandleNames[UIRectEdit::kHandleCount] = { "tl", "tr", "br", "bl", "t", "r", "b", "l", "body" };
    // kAnchorTL .. kPivotHandle, then kSliceL .. kSliceB, in order.
    const char* const kExtraHandleNames[10] = { "anchor_min", "anchor_tr", "anchor_max", "anchor_bl", "anchor", "pivot",
                                                "slice_l", "slice_t", "slice_r", "slice_b" };
}

int UIEditorWindow::HandleFromName(const std::string& nameIn)
{
    const std::string name = Lower(nameIn);
    for (int i = 0; i < UIRectEdit::kHandleCount; ++i)
        if (name == kRectHandleNames[i]) return i;
    for (int i = 0; i < 10; ++i)
        if (name == kExtraHandleNames[i]) return kAnchorTL + i;
    return -1;
}

const char* UIEditorWindow::HandleName(int handle)
{
    if (handle >= 0 && handle < UIRectEdit::kHandleCount) return kRectHandleNames[handle];
    if (handle >= kAnchorTL && handle <= kSliceB) return kExtraHandleNames[handle - kAnchorTL];
    return "?";
}

void UIEditorWindow::RebuildRows(const UIEditorContext& ctx)
{
    m_rows.clear();
    m_rowsTab   = m_active;
    m_rowsFrame = m_frame;
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) return;
    std::vector<UI::LayoutNode> nodes;
    float scale = 1.f;
    UI::Vec2 design{ 0.f, 0.f };
    std::string why;
    if (!Layout(ctx, nodes, scale, design, why)) return;
    m_rowsScale  = scale;
    m_rowsDesign = design;
    World& w = *ctx.world;
    const UI::Rect screen{ UI::Vec2{ 0.f, 0.f }, design };
    std::unordered_map<Entity, std::size_t> index;
    m_rows.reserve(nodes.size());
    auto has = [](const std::vector<Entity>& v, Entity e) { return std::find(v.begin(), v.end(), e) != v.end(); };
    for (const UI::LayoutNode& n : nodes)
    {
        Row r;
        r.e      = n.e;
        r.rect   = n.cr.rect;
        r.clip   = n.cr.clipRect;
        r.depth  = n.cr.depth;
        r.culled = n.cr.culled;
        r.hidden = n.hidden;
        r.isRoot = n.e == t->subject;
        if (const UI::UIRect* ur = w.GetComponent<UI::UIRect>(n.e))
        {
            r.isRect   = true;
            r.authored = *ur;
        }
        r.parent = r.isRoot ? NullEntity : UI::ResolveUIParent(w, n.e);
        const auto it = index.find(r.parent);
        r.parentRect = it != index.end() ? m_rows[it->second].rect : screen;
        // Hidden in the designer goes down the subtree; the lock does not.
        r.skip   = has(t->designHidden, n.e) || (it != index.end() && m_rows[it->second].skip);
        r.locked = has(t->locked, n.e);
        if (it != index.end()) m_rows[it->second].hasKids = true;
        index[n.e] = m_rows.size();
        m_rows.push_back(r);
    }
}

const UIEditorWindow::Row* UIEditorWindow::FindRow(Entity e) const
{
    if (e == NullEntity) return nullptr;
    for (const Row& r : m_rows)
        if (r.e == e) return &r;
    return nullptr;
}

Entity UIEditorWindow::HitTest(const UIEditorContext& ctx, UI::Vec2 p, bool cycle, Entity current)
{
    (void)ctx;
    const Tab* t = ActiveTabPtr();
    if (!t) return NullEntity;
    // Front to back = reverse paint order; every node, not only raycast targets.
    std::vector<Entity> hits;
    for (auto it = m_rows.rbegin(); it != m_rows.rend(); ++it)
    {
        const Row& r = *it;
        if (r.isRoot || r.culled || (r.hidden && !t->view.showHidden) || r.skip || r.locked) continue;
        const UI::Rect vis = UI::IntersectRects(r.rect, r.clip);
        if (p.x >= vis.mn.x && p.x < vis.mx.x && p.y >= vis.mn.y && p.y < vis.mx.y) hits.push_back(r.e);
    }
    if (hits.empty()) return t->subject;   // empty space: the canvas itself (UE)
    if (!cycle) return hits.front();
    // Alt+click: the next node under the point behind the current one.
    const auto it = std::find(hits.begin(), hits.end(), current);
    return (it == hits.end() || it + 1 == hits.end()) ? hits.front() : *(it + 1);
}

bool UIEditorWindow::Select(const UIEditorContext& ctx, Entity e, std::string& why)
{
    if (!ActiveTabPtr()) { why = "no canvas open"; return false; }
    RebuildRows(ctx);   // a scripted call: the node may be this frame's
    if (e != NullEntity && !FindRow(e)) { why = "not a node of the open canvas"; return false; }
    SelectNode(ctx, e);
    return true;
}

bool UIEditorWindow::BeginDrag(const UIEditorContext& ctx, Entity e, int handle, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx))
    {
        why = FrozenWhy(ctx);
        return false;
    }
    if (InstanceNode(ctx, e, &why)) return false;
    if ((handle < 0 || handle >= UIRectEdit::kHandleCount) && (handle < kAnchorTL || handle > kSliceB))
    {
        why = "no such handle";
        return false;
    }
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return false; }
    if (!r->isRect) { why = "the canvas root has no rect to drag"; return false; }
    if (r->culled) { why = "the node is not laid out (hidden, or clipped away)"; return false; }
    UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(e);
    if (!ur) { why = "the node has no UIRect"; return false; }
    const bool slice = handle >= kSliceL;
    const UI::UIImage* img = slice ? ctx.world->GetComponent<UI::UIImage>(e) : nullptr;
    if (slice && (!img || img->texturePath.empty()))
    {
        why = "a 9-slice border needs a Canvas Image with a texture";   // what the overlay offers
        return false;
    }
    m_drag              = DragState{};
    m_drag.active       = true;
    m_drag.e            = e;
    m_drag.handle       = handle;
    m_drag.start        = *ur;
    m_drag.startRect    = r->rect;
    m_drag.rctx.parent  = r->parentRect;
    m_drag.rctx.hasParent = true;
    m_drag.rctx.scale   = m_rowsScale;
    m_drag.slice        = slice;
    if (img)
    {
        m_drag.startBorder[0] = img->sliceBorder.x;
        m_drag.startBorder[1] = img->sliceBorder.y;
        m_drag.startBorder[2] = img->sliceBorder.z;
        m_drag.startBorder[3] = img->sliceBorder.w;
    }
    m_drag.beforeBlob   = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, e,
                                                      slice ? std::type_index(typeid(UI::UIImage))
                                                            : std::type_index(typeid(UI::UIRect)));
    // Snap targets: the parent's edges and centre, every laid-out sibling's.
    auto add = [](std::vector<float>& v, float a, float b) { v.push_back(a); v.push_back((a + b) * 0.5f); v.push_back(b); };
    add(m_drag.snapX, r->parentRect.mn.x, r->parentRect.mx.x);
    add(m_drag.snapY, r->parentRect.mn.y, r->parentRect.mx.y);
    for (const Row& s : m_rows)
        if (s.parent == r->parent && s.e != e && !s.culled && !s.isRoot)
        {
            add(m_drag.snapX, s.rect.mn.x, s.rect.mx.x);
            add(m_drag.snapY, s.rect.mn.y, s.rect.mx.y);
        }
    return true;
}

void UIEditorWindow::UpdateDrag(const UIEditorContext& ctx, UI::Vec2 d, bool ctrl, bool shift, bool alt)
{
    if (!m_drag.active || !ctx.world) return;
    UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(m_drag.e);
    if (!ur) { m_drag = DragState{}; return; }
    m_drag.hasGuideX = m_drag.hasGuideY = false;
    // No movement yet: nothing (a click must not nudge the node or record an edit).
    if (d.x == 0.f && d.y == 0.f) return;
    const Tab* t = ActiveTabPtr();
    if (m_drag.slice)
    {
        // One 9-slice border. Texture pixels ARE destination pixels (EmitNineSlice
        // draws a corner sliceBorder px wide), so a design-px delta is the change.
        UI::UIImage* img = ctx.world->GetComponent<UI::UIImage>(m_drag.e);
        if (!img) { m_drag = DragState{}; return; }
        const UI::Rect& r0 = m_drag.startRect;
        const float w = r0.mx.x - r0.mn.x, h = r0.mx.y - r0.mn.y;
        float b[4] = { m_drag.startBorder[0], m_drag.startBorder[1], m_drag.startBorder[2], m_drag.startBorder[3] };
        auto set = [&](int i, float v, float span, int opposite)
        {
            v = ctrl ? std::round(v) : v;
            b[i] = std::clamp(v, 0.f, (std::max)(0.f, span - b[opposite]));
        };
        switch (m_drag.handle)
        {
        case kSliceL: set(0, b[0] + d.x, w, 2); break;
        case kSliceR: set(2, b[2] - d.x, w, 0); break;
        case kSliceT: set(1, b[1] + d.y, h, 3); break;
        default:      set(3, b[3] - d.y, h, 1); break;
        }
        img->sliceBorder = { b[0], b[1], b[2], b[3] };
        m_drag.moved = true;
        return;
    }
    if (m_drag.handle >= kAnchorTL)
    {
        // The anchors / the pivot move; offset and size are solved so the rect
        // stays exactly where it is (keep-visual). Values snap to 0, 0.5 and 1.
        float z = 1.f, pan[2];
        if (t) EffectiveView(*t, t->designW, t->designH, z, pan);
        const float thr = (t && t->view.snap && !alt) ? kSnapViewPx / (std::max)(z, 1e-3f) : 0.f;
        const UI::UIRect& s0 = m_drag.start;
        if (m_drag.handle == kPivotHandle)
        {
            const UI::Rect& r0 = m_drag.startRect;
            const float w = (std::max)(1e-3f, r0.mx.x - r0.mn.x), h = (std::max)(1e-3f, r0.mx.y - r0.mn.y);
            UI::Vec2 piv{ std::clamp(std::clamp(s0.pivot.x, 0.f, 1.f) + d.x / w, 0.f, 1.f),
                          std::clamp(std::clamp(s0.pivot.y, 0.f, 1.f) + d.y / h, 0.f, 1.f) };
            piv.x = SnapUnit(piv.x, thr / w);
            piv.y = SnapUnit(piv.y, thr / h);
            *ur = UIRectEdit::SetPivotKeepRect(s0, piv, m_drag.startRect, m_drag.rctx);
        }
        else
        {
            const UI::Rect& p = m_drag.rctx.parent;
            const float pw = (std::max)(1.f, p.mx.x - p.mn.x), ph = (std::max)(1.f, p.mx.y - p.mn.y);
            const float dx = d.x / pw, dy = d.y / ph;
            UI::Vec2 aMin = s0.anchorMin, aMax = s0.anchorMax;
            const int hd = m_drag.handle;
            const bool minX = hd == kAnchorTL || hd == kAnchorBL || hd == kAnchorAll;
            const bool maxX = hd == kAnchorTR || hd == kAnchorBR || hd == kAnchorAll;
            const bool minY = hd == kAnchorTL || hd == kAnchorTR || hd == kAnchorAll;
            const bool maxY = hd == kAnchorBL || hd == kAnchorBR || hd == kAnchorAll;
            auto move = [&](float& v, float dv, float tu) { v = std::clamp(SnapUnit(std::clamp(v + dv, 0.f, 1.f), tu), 0.f, 1.f); };
            if (minX) move(aMin.x, dx, thr / pw);
            if (maxX) move(aMax.x, dx, thr / pw);
            if (minY) move(aMin.y, dy, thr / ph);
            if (maxY) move(aMax.y, dy, thr / ph);
            // The corners may meet but never cross: the one being moved stops.
            if (aMin.x > aMax.x) { if (minX && !maxX) aMin.x = aMax.x; else aMax.x = aMin.x; }
            if (aMin.y > aMax.y) { if (minY && !maxY) aMin.y = aMax.y; else aMax.y = aMin.y; }
            *ur = UIRectEdit::SetAnchorsKeepRect(s0, aMin, aMax, m_drag.startRect, m_drag.rctx);
        }
        m_drag.moved = true;
        return;
    }
    if (t && t->view.snap && !alt)
    {
        float z = 1.f, pan[2];
        EffectiveView(*t, t->designW, t->designH, z, pan);
        const float thr = kSnapViewPx / (std::max)(z, 1e-3f);   // design px
        const UIRectEdit::Edges h = UIRectEdit::EdgesOf(m_drag.handle);
        const UI::Rect& s0 = m_drag.startRect;
        const float g = static_cast<float>((std::max)(1, t->view.gridRef)) * m_rowsScale;
        auto snap = [&](float& dv, float lo, float hi, float el, float eh, const std::vector<float>& targets,
                        float& guide, bool& has)
        {
            // The features that move: all three for the body, the dragged edge otherwise.
            float self[3];
            if (el > 0.f && eh > 0.f) { self[0] = lo; self[1] = (lo + hi) * 0.5f; self[2] = hi; }
            else if (el > 0.f)        { self[0] = self[1] = self[2] = lo; }
            else if (eh > 0.f)        { self[0] = self[1] = self[2] = hi; }
            else return;
            const float hit = UIRectEdit::SnapAxis(dv, self, targets.data(), static_cast<int>(targets.size()), thr);
            if (!std::isnan(hit)) { guide = hit; has = true; return; }
            if (!t->view.grid) return;
            for (const float f : self)   // the grid, as a last resort
            {
                const float line = std::round((f + dv) / g) * g;
                if (std::fabs(f + dv - line) <= thr) { dv = line - f; guide = line; has = true; return; }
            }
        };
        snap(d.x, s0.mn.x, s0.mx.x, h.l, h.r, m_drag.snapX, m_drag.guideX, m_drag.hasGuideX);
        snap(d.y, s0.mn.y, s0.mx.y, h.t, h.b, m_drag.snapY, m_drag.guideY, m_drag.hasGuideY);
    }
    UIRectEdit::DragOptions o;
    o.quantise     = ctrl;
    o.proportional = shift;
    *ur = UIRectEdit::ApplyHandleDrag(m_drag.start, m_drag.handle, d, m_drag.rctx, o);
    m_drag.moved = true;
}

void UIEditorWindow::EndDrag(const UIEditorContext& ctx)
{
    if (!m_drag.active) return;
    if (ctx.world && ctx.world->IsAlive(m_drag.e) && m_drag.moved)
    {
        const std::type_index type = m_drag.slice ? std::type_index(typeid(UI::UIImage))
                                                  : std::type_index(typeid(UI::UIRect));
        const std::string after = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, m_drag.e, type);
        if (!after.empty() && after != m_drag.beforeBlob)
        {
            if (ctx.pushEdit)
                ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(
                    ctx.world->MakeHandle(m_drag.e), type, m_drag.beforeBlob, after,
                    m_drag.slice ? "Canvas Image" : "UI Rect"));
            if (m_drag.slice)
            {
                if (const UI::UIImage* img = ctx.world->GetComponent<UI::UIImage>(m_drag.e))
                    LOG_INFO("[UIEDITOR] slice %s handle=%s border=%g,%g,%g,%g->%g,%g,%g,%g",
                             ctx.world->GetName(m_drag.e).c_str(), HandleName(m_drag.handle), m_drag.startBorder[0],
                             m_drag.startBorder[1], m_drag.startBorder[2], m_drag.startBorder[3], img->sliceBorder.x,
                             img->sliceBorder.y, img->sliceBorder.z, img->sliceBorder.w);
            }
            else if (const UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(m_drag.e))
                LOG_INFO("[UIEDITOR] drag %s handle=%s scale=%.4f offset=%g,%g->%g,%g size=%g,%g->%g,%g",
                         ctx.world->GetName(m_drag.e).c_str(), HandleName(m_drag.handle), m_drag.rctx.scale,
                         m_drag.start.offset.x, m_drag.start.offset.y, ur->offset.x, ur->offset.y,
                         m_drag.start.size.x, m_drag.start.size.y, ur->size.x, ur->size.y);
        }
    }
    m_drag = DragState{};
}

bool UIEditorWindow::Drag(const UIEditorContext& ctx, int handle, UI::Vec2 delta, bool ctrl, bool shift, bool alt,
                          std::string& why)
{
    if (!ActiveTabPtr()) { why = "no canvas open"; return false; }
    RebuildRows(ctx);
    const Entity sel = Sel(ctx);
    if (sel == NullEntity) { why = "nothing is selected"; return false; }
    if (!BeginDrag(ctx, sel, handle, why)) return false;
    UpdateDrag(ctx, delta, ctrl, shift, alt);
    EndDrag(ctx);
    return true;
}

void UIEditorWindow::NudgeSelection(const UIEditorContext& ctx, float dx, float dy)
{
    if (!ctx.world || Frozen(ctx)) return;
    const Entity e = Sel(ctx);
    if (InstanceNode(ctx, e)) return;
    const Row* r = FindRow(e);
    UI::UIRect* ur = (r && r->isRect) ? ctx.world->GetComponent<UI::UIRect>(e) : nullptr;
    if (!ur) return;
    if (!m_nudge.active || m_nudge.e != e)
    {
        EndNudge(ctx);
        m_nudge.active     = true;
        m_nudge.e          = e;
        m_nudge.beforeBlob = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, e,
                                                         std::type_index(typeid(UI::UIRect)));
    }
    ur->offset.x += dx;
    ur->offset.y += dy;
}

void UIEditorWindow::EndNudge(const UIEditorContext& ctx)
{
    if (!m_nudge.active) return;
    if (ctx.world && ctx.world->IsAlive(m_nudge.e) && ctx.pushEdit)
    {
        const std::string after = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, m_nudge.e,
                                                              std::type_index(typeid(UI::UIRect)));
        if (!after.empty() && after != m_nudge.beforeBlob)
            ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(
                ctx.world->MakeHandle(m_nudge.e), std::type_index(typeid(UI::UIRect)), m_nudge.beforeBlob, after,
                "UI Rect"));
    }
    m_nudge = Nudge{};
}

void UIEditorWindow::FrameSelection(const UIEditorContext& ctx, Tab& t)
{
    const Row* r = FindRow(Sel(ctx));
    if (!r || r->isRoot) { t.view.zoom = 0.f; return; }   // nothing (or the canvas): fit
    const float cw = m_pinW ? static_cast<float>(m_pinW) : (m_canvasW > 0.f ? m_canvasW : float(kDefaultBoxW));
    const float ch = m_pinH ? static_cast<float>(m_pinH) : (m_canvasH > 0.f ? m_canvasH : float(kDefaultBoxH));
    const float w = (std::max)(1.f, r->rect.mx.x - r->rect.mn.x), h = (std::max)(1.f, r->rect.mx.y - r->rect.mn.y);
    const float z = std::clamp((std::min)(cw * 0.6f / w, ch * 0.6f / h), kMinZoom, kMaxZoom);
    t.view.zoom   = z;
    t.view.pan[0] = std::floor(cw * 0.5f - (r->rect.mn.x + w * 0.5f) * z);
    t.view.pan[1] = std::floor(ch * 0.5f - (r->rect.mn.y + h * 0.5f) * z);
}

// ==== Per-canvas resolution (editor_prefs.ini) ====
void UIEditorWindow::LoadPref(const std::string& key, const std::string& value)
{
    // The side columns.
    if (key == "uieditor.leftW" || key == "uieditor.rightW")
    {
        const float v = static_cast<float>(std::atof(value.c_str()));
        if (std::isfinite(v) && v >= 150.f && v <= 900.f) (key == "uieditor.leftW" ? m_leftW : m_rightW) = v;
        return;
    }
    if (key == "uieditor.panels") { m_panels = value != "0"; return; }
    static const char* kPrefix = "uieditor.res.";
    if (key.rfind(kPrefix, 0) != 0 || value.empty()) return;
    const std::string guid = key.substr(std::strlen(kPrefix));
    for (auto& [g, v] : m_resPrefs)
        if (g == guid) { v = value; return; }
    m_resPrefs.emplace_back(guid, value);
}

void UIEditorWindow::WritePrefs(std::ostream& out) const
{
    // Only what differs from the default (the reference resolution): the file is
    // tracked, and a key per canvas ever opened would be a diff nobody asked for.
    for (const auto& [g, v] : m_resPrefs)
        if (!g.empty() && v != "reference") out << "uieditor.res." << g << '=' << v << '\n';
    if (std::fabs(m_leftW - 240.f) > 0.5f)  out << "uieditor.leftW=" << static_cast<int>(m_leftW) << '\n';
    if (std::fabs(m_rightW - 340.f) > 0.5f) out << "uieditor.rightW=" << static_cast<int>(m_rightW) << '\n';
    if (!m_panels)                          out << "uieditor.panels=0\n";
}

void UIEditorWindow::ApplyResPref(Tab& t) const
{
    if (!t.guid.IsValid()) return;
    const std::string g = HexGuid(t.guid);
    for (const auto& [k, v] : m_resPrefs)
        if (k == g)
        {
            std::uint32_t w = 0, h = 0;
            if (v == "match_main") t.view.res = Resolution::MatchMain;
            else if (ParseSize(v, w, h) && w >= kMinDesign && h >= kMinDesign && w <= kMaxDesign && h <= kMaxDesign)
            {
                t.view.res  = Resolution::Custom;
                t.view.resW = w;
                t.view.resH = h;
            }
            return;
        }
}

void UIEditorWindow::RememberRes(const Tab& t)
{
    // A scene canvas's only: a stage's root GUID is new at every spawn (a key per
    // scene load / Revert / reload would pile up in editor_prefs.ini).
    if (t.kind != TabKind::Scene || !t.guid.IsValid()) return;
    const std::string g = HexGuid(t.guid);
    std::string v = "reference";
    if (t.view.res == Resolution::MatchMain) v = "match_main";
    else if (t.view.res == Resolution::Custom) v = std::to_string(t.view.resW) + "x" + std::to_string(t.view.resH);
    for (auto& [k, val] : m_resPrefs)
        if (k == g) { val = v; return; }
    m_resPrefs.emplace_back(g, v);
}

// ==== Anchors and the pivot ====
bool UIEditorWindow::IsAnchorPreset(const std::string& name)
{
    for (const AnchorPreset& p : kAnchorPresets)
        if (name == p.name) return true;
    return false;
}

bool UIEditorWindow::ApplyAnchorPreset(const UIEditorContext& ctx, const std::string& name, bool setPivot,
                                       bool setPosition, bool keepVisual, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    const AnchorPreset* pr = nullptr;
    for (const AnchorPreset& p : kAnchorPresets)
        if (name == p.name) pr = &p;
    if (!pr) { why = "no anchor preset '" + name + "' (top_left .. stretch_full)"; return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(Sel(ctx));
    if (!r || !r->isRect) { why = r ? "the canvas root has no anchors" : "nothing of the open canvas is selected"; return false; }
    if (InstanceNode(ctx, r->e, &why)) return false;
    UI::UIRect* ur = ctx.world->GetComponent<UI::UIRect>(r->e);
    if (!ur) { why = "the node has no UIRect"; return false; }
    const std::string before = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, r->e,
                                                            std::type_index(typeid(UI::UIRect)));
    UIRectEdit::Ctx rc;
    rc.parent    = r->parentRect;
    rc.hasParent = true;
    rc.scale     = m_rowsScale;
    const Entity   e     = r->e;
    const UI::Rect rect0 = r->rect;
    // -uieditor_anchor_raw (the uieditor_anchor pack's reverse arm): the solve is
    // skipped as if it were broken - the request (and the log line) still says keep-visual.
    const bool solve = keepVisual && !m_harnessAnchorRaw;
    UI::UIRect n = *ur;
    const UI::Vec2 aMin{ pr->minX, pr->minY }, aMax{ pr->maxX, pr->maxY };
    if (solve) n = UIRectEdit::SetAnchorsKeepRect(n, aMin, aMax, rect0, rc);
    else { n.anchorMin = aMin; n.anchorMax = aMax; }
    if (setPivot)
    {
        const UI::Vec2 piv{ pr->pivX, pr->pivY };
        if (solve) n = UIRectEdit::SetPivotKeepRect(n, piv, UIRectEdit::Resolve(n, rc), rc);
        else n.pivot = piv;
    }
    if (setPosition)
    {
        // Onto the anchors: the pivot sits on a point anchor, a stretched axis fills its span.
        n.offset = { 0.f, 0.f };
        if (pr->minX != pr->maxX) n.size.x = 0.f;
        if (pr->minY != pr->maxY) n.size.y = 0.f;
    }
    *ur = n;
    const std::string after = Undo::SerializeComponentBlob(GetComponentRegistry(), *ctx.world, e,
                                                          std::type_index(typeid(UI::UIRect)));
    if (ctx.pushEdit && after != before)
        ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(ctx.world->MakeHandle(e),
                                                                std::type_index(typeid(UI::UIRect)), before, after,
                                                                "UI Rect"));
    // The rect it lands on, laid out again: keep-visual means the same numbers.
    RebuildRows(ctx);
    const Row*     r1    = FindRow(e);
    const UI::Rect rect1 = r1 ? r1->rect : UI::Rect{};
    LOG_INFO("[UIEDITOR] anchors %s -> %s pivot=%d position=%d keepVisual=%d rect=%.2f,%.2f,%.2f,%.2f->%.2f,%.2f,%.2f,%.2f",
             ctx.world->GetName(e).c_str(), pr->name, setPivot ? 1 : 0, setPosition ? 1 : 0, keepVisual ? 1 : 0,
             rect0.mn.x, rect0.mn.y, rect0.mx.x - rect0.mn.x, rect0.mx.y - rect0.mn.y, rect1.mn.x, rect1.mn.y,
             rect1.mx.x - rect1.mn.x, rect1.mx.y - rect1.mn.y);
    return true;
}

void UIEditorWindow::DrawAnchorPresetPopup(const UIEditorContext& ctx)
{
    if (!ImGui::BeginPopup("##uieanchors")) return;
    ImGui::TextDisabled("Anchors   (Alt: + pivot   Shift: + position)");
    const ImGuiIO& io = ImGui::GetIO();
    static const char* kRow[4] = { "Top", "Middle", "Bottom", "Stretch" };
    static const char* kCol[4] = { "Left", "Center", "Right", "Stretch" };
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            const AnchorPreset& p = kAnchorPresets[row * 4 + col];
            if (col) ImGui::SameLine();
            ImGui::PushID(row * 4 + col);
            // A tiny picture of the preset: the parent box and where the anchors sit.
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const float  s = 34.f;
            if (ImGui::InvisibleButton("##p", ImVec2(s, s)))
            {
                std::string why;
                if (!ApplyAnchorPreset(ctx, p.name, io.KeyAlt, io.KeyShift, true, why))
                    LOG_WARNING("[UIEDITOR] anchor preset %s: %s", p.name, why.c_str());
                ImGui::CloseCurrentPopup();
            }
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRect(ImVec2(o.x + 4.f, o.y + 4.f), ImVec2(o.x + s - 4.f, o.y + s - 4.f),
                        hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 160, 255));
            const float w = s - 8.f;
            const ImVec2 a(o.x + 4.f + p.minX * w, o.y + 4.f + p.minY * w), b(o.x + 4.f + p.maxX * w, o.y + 4.f + p.maxY * w);
            dl->AddRectFilled(ImVec2(a.x - 2.f, a.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f), IM_COL32(255, 200, 60, 230));
            if (hov) ImGui::SetTooltip("%s %s", kRow[row], kCol[col]);
            ImGui::PopID();
        }
    }
    ImGui::EndPopup();
}

// ==== The panels: Palette, Hierarchy, Details, Check ====
namespace
{

    std::string Blob(World& w, Entity e, std::type_index type)
    {
        return Undo::SerializeComponentBlob(GetComponentRegistry(), w, e, type);
    }

    // The GUID a create stamped on a parent that had none, recorded inside the
    // create's macro: its undo then leaves the scene exactly as it was.
    void RecordGuidStamp(const UIEditorContext& ctx, Entity parent)
    {
        if (!ctx.world || !ctx.pushEdit) return;
        if (auto a = UITemplates::GuidStampAction(*ctx.world, parent)) ctx.pushEdit(std::move(a));
    }
}

Entity UIEditorWindow::DropParent(const UIEditorContext& ctx, UI::Vec2 at)
{
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) return NullEntity;
    Entity hit = HitTest(ctx, at);   // skips locked and designer-hidden nodes
    // Text is a leaf: a drop onto a label goes beside it.
    if (hit != t->subject && ctx.world->HasComponent<UI::UIText>(hit))
        if (const Row* r = FindRow(hit)) hit = r->parent != NullEntity ? r->parent : t->subject;
    return hit;
}

Entity UIEditorWindow::CreateWidget(const UIEditorContext& ctx, const std::string& type, Entity parent, bool hasAt,
                                    UI::Vec2 at, std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) { why = "no canvas open"; return NullEntity; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return NullEntity; }
    World& w = *ctx.world;
    UITemplates::Widget widget{};
    const bool builtIn = UITemplates::FromName(type, widget);
    const std::string ext = builtIn ? std::string() : Lower(Sys::PathToUtf8(Sys::U8Path(type).extension()));
    // A .iui is spawned here, through the codec Engine.SpawnUI uses.
    std::shared_ptr<const UI::UIAsset> uiAsset;
    if (!builtIn)
    {
        if (ext == ".iui")
        {
            uiAsset = UI::LoadUIAsset(type, &why);
            if (!uiAsset) return NullEntity;
            if (uiAsset->root.Block("CvCanvas"))
            {
                why = "'" + type + "' is a canvas: it is a root of its own, not a widget to place";
                return NullEntity;
            }
        }
        else if (ext != ".ipfb")
        {
            why = "'" + type + "' is no widget (panel, image, text, button, sprite, clip_panel, empty_rect) and no "
                  ".ipfb / .iui UI template";
            return NullEntity;
        }
        else if (!ctx.instantiate) { why = "this editor cannot instantiate templates"; return NullEntity; }
    }
    RebuildRows(ctx);
    if (!hasAt && parent == NullEntity)
    {
        // The selection's container: the node itself (a text node's parent - text is
        // a leaf), else the canvas.
        parent = t->subject;
        if (const Row* s = FindRow(Sel(ctx)); s && !s->isRoot)
            parent = w.HasComponent<UI::UIText>(s->e) ? (s->parent != NullEntity ? s->parent : t->subject) : s->e;
    }
    else if (parent == NullEntity)
        parent = DropParent(ctx, at);
    const Row* pr = FindRow(parent);
    if (!pr) { why = "the parent is not a node of the open canvas"; return NullEntity; }
    if (InstanceNode(ctx, parent, &why)) return NullEntity;   // nothing goes into an instance
    const UI::Rect parentRect = pr->rect;
    const float    scale      = m_rowsScale;
    Entity      made = NullEntity;
    std::string refusal;
    const std::string label = builtIn ? std::string("UI ") + UITemplates::Label(widget) : std::string("UI Template");
    auto spawn = [&]
    {
        if (builtIn) made = UITemplates::Create(w, widget, parent);
        else if (uiAsset)
        {
            UI::UISpawnOptions o;
            o.parent   = parent;
            o.assetKey = Sys::RelativizeAssetPath(type);
            made = UI::SpawnUIAsset(w, *uiAsset, o, &refusal);
        }
        else made = ctx.instantiate(type, parent, &refusal);
        if (made != NullEntity && hasAt) UITemplates::PlacePivotAt(w, made, at, parentRect, scale);
    };
    const bool hadGuid = w.HasComponent<GuidComponent>(parent);
    if (ctx.beginMacro) ctx.beginMacro(label);
    if (ctx.recordSpawn) ctx.recordSpawn(label.c_str(), spawn);
    else spawn();
    if (made != NullEntity && !hadGuid) RecordGuidStamp(ctx, parent);
    if (ctx.endMacro) ctx.endMacro();
    if (made == NullEntity)
    {
        why = refusal.empty() ? "nothing was made" : refusal;
        return NullEntity;
    }
    SelectNode(ctx, made);
    char where[64] = "centre";
    if (hasAt) std::snprintf(where, sizeof(where), "%g,%g", at.x, at.y);
    LOG_INFO("[UIEDITOR] create %s '%s' under '%s' at=%s", builtIn ? UITemplates::Name(widget) : type.c_str(),
             w.GetName(made).c_str(), w.GetName(parent).c_str(), where);
    return made;
}

bool UIEditorWindow::Reorder(const UIEditorContext& ctx, Entity e, int delta, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return false; }
    if (r->isRoot) { why = "the canvas has no siblings"; return false; }
    if (InstanceNode(ctx, e, &why)) return false;
    std::vector<Entity> sibs;
    UI::UIChildrenOf(*ctx.world, r->parent, sibs);
    const int at = static_cast<int>(std::find(sibs.begin(), sibs.end(), e) - sibs.begin());
    const int to = std::clamp(at + delta, 0, (std::max)(0, static_cast<int>(sibs.size()) - 1));
    if (to == at) return true;
    if (!ctx.moveUINode) { why = "this editor cannot move UI nodes"; return false; }
    return ctx.moveUINode(e, r->parent, to, &why);
}

bool UIEditorWindow::Reparent(const UIEditorContext& ctx, Entity e, Entity parent, int index, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "refused: no canvas open"; return false; }
    if (Frozen(ctx)) { why = "refused: " + FrozenWhy(ctx); return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r || !FindRow(parent)) { why = "refused: not a node of the open canvas"; return false; }
    if (r->isRoot) { why = "refused: the root has no parent to change"; return false; }
    if (!ctx.moveUINode) { why = "refused: this editor cannot move UI nodes"; return false; }
    // The rest (a cycle, an instance node, a canvas) is MoveUINode's, as for the drop.
    return ctx.moveUINode(e, parent, index, &why);
}

bool UIEditorWindow::SetDesignerFlag(const UIEditorContext& ctx, Entity e, bool locked, bool on, std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) { why = "no canvas open"; return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return false; }
    if (r->isRoot) { why = "the canvas itself is never hidden or locked"; return false; }
    if (SetIn(locked ? t->locked : t->designHidden, e, on))
    {
        LOG_INFO("[UIEDITOR] %s %s %s", locked ? "lock" : "hide", ctx.world->GetName(e).c_str(), on ? "on" : "off");
        RebuildRows(ctx);
    }
    return true;
}

bool UIEditorWindow::DesignerHidden(Entity e) const
{
    const Tab* t = ActiveTabPtr();
    return t && HasIn(t->designHidden, e);
}

bool UIEditorWindow::Locked(Entity e) const
{
    const Tab* t = ActiveTabPtr();
    return t && HasIn(t->locked, e);
}

bool UIEditorWindow::SetText(const UIEditorContext& ctx, Entity e, const std::string& text, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    RebuildRows(ctx);
    if (!FindRow(e)) { why = "not a node of the open canvas"; return false; }
    if (InstanceNode(ctx, e, &why)) return false;
    UI::UIText* tx = ctx.world->GetComponent<UI::UIText>(e);
    if (!tx) { why = "the node has no Canvas Text"; return false; }
    if (tx->text == text) return true;
    const std::type_index type(typeid(UI::UIText));
    const std::string before = Blob(*ctx.world, e, type);
    tx->text = text;
    const std::string after = Blob(*ctx.world, e, type);
    if (ctx.pushEdit && after != before)
        ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(ctx.world->MakeHandle(e), type, before, after,
                                                                "Canvas Text"));
    LOG_INFO("[UIEDITOR] text %s: %zu bytes", ctx.world->GetName(e).c_str(), text.size());
    return true;
}

Entity UIEditorWindow::DropTexture(const UIEditorContext& ctx, const std::string& pathIn, UI::Vec2 at, std::string& why)
{
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) { why = "no canvas open"; return NullEntity; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return NullEntity; }
    if (pathIn.empty()) { why = "no texture path"; return NullEntity; }
    World& w = *ctx.world;
    const std::string path = Sys::RelativizeAssetPath(pathIn);
    RebuildRows(ctx);
    const Entity hit = HitTest(ctx, at);
    if (InstanceNode(ctx, hit, &why)) return NullEntity;
    if (hit != t->subject)
        if (UI::UIImage* img = w.GetComponent<UI::UIImage>(hit))
        {
            // Onto an image: its texture (one undo step).
            const std::type_index type(typeid(UI::UIImage));
            const std::string before = Blob(w, hit, type);
            img->texturePath = path;
            const std::string after = Blob(w, hit, type);
            if (ctx.pushEdit && after != before)
                ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(w.MakeHandle(hit), type, before, after,
                                                                        "Canvas Image"));
            LOG_INFO("[UIEDITOR] texture %s -> '%s'", path.c_str(), w.GetName(hit).c_str());
            return hit;
        }
    // Elsewhere: a new Image there, at the texture's size (one reference px a texel).
    const Resource::TextureFileInfo info = Resource::DescribeTextureFile(path);
    if (!info.ok) { why = "'" + path + "' is not a readable texture (" + info.error + ")"; return NullEntity; }
    const Entity parent = DropParent(ctx, at);
    const Row* pr = FindRow(parent);
    if (!pr) { why = "nothing to put the image under"; return NullEntity; }
    const UI::Rect parentRect = pr->rect;
    const float    scale      = m_rowsScale;
    Entity made = NullEntity;
    auto spawn = [&]
    {
        made = UITemplates::Create(w, UITemplates::Widget::Image, parent);
        if (made == NullEntity) return;
        if (UI::UIImage* img = w.GetComponent<UI::UIImage>(made)) img->texturePath = path;
        if (UI::UIRect* r = w.GetComponent<UI::UIRect>(made))
            r->size = { static_cast<float>(info.width), static_cast<float>(info.height) };
        const std::string stem = Sys::PathToUtf8(Sys::U8Path(path).stem());
        if (!stem.empty()) w.SetName(made, stem);
        UITemplates::PlacePivotAt(w, made, at, parentRect, scale);
    };
    const bool hadGuid = w.HasComponent<GuidComponent>(parent);
    if (ctx.beginMacro) ctx.beginMacro("UI Image");
    if (ctx.recordSpawn) ctx.recordSpawn("UI Image", spawn);
    else spawn();
    if (made != NullEntity && !hadGuid) RecordGuidStamp(ctx, parent);
    if (ctx.endMacro) ctx.endMacro();
    if (made == NullEntity) { why = "the image could not be made"; return NullEntity; }
    SelectNode(ctx, made);
    LOG_INFO("[UIEDITOR] texture %s -> new image '%s' %ux%u under '%s'", path.c_str(), w.GetName(made).c_str(),
             info.width, info.height, w.GetName(parent).c_str());
    return made;
}

bool UIEditorWindow::NativeSize(const UIEditorContext& ctx, Entity e, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return false; }
    if (r->isRoot || !r->isRect) { why = "the canvas has no size of its own"; return false; }
    if (InstanceNode(ctx, e, &why)) return false;
    World& w = *ctx.world;
    const UI::UIImage* img = w.GetComponent<UI::UIImage>(e);
    UI::UIRect* ur = w.GetComponent<UI::UIRect>(e);
    if (!img || !ur) { why = "the node has no Canvas Image"; return false; }
    float tw = img->texWidth, th = img->texHeight;
    if ((tw <= 0.f || th <= 0.f) && !img->texturePath.empty())
    {
        const Resource::TextureFileInfo info = Resource::DescribeTextureFile(img->texturePath);
        if (info.ok) { tw = static_cast<float>(info.width); th = static_cast<float>(info.height); }
    }
    if (tw <= 0.f || th <= 0.f)
    {
        why = img->texturePath.empty() ? "the image has no texture" : "'" + img->texturePath + "' cannot be read";
        return false;
    }
    // The texels the image shows: its uv window - a sprite sheet's one frame (the
    // animation rewrites the uv every frame; its grid is what holds).
    float nw = tw * std::fabs(img->uv1.x - img->uv0.x), nh = th * std::fabs(img->uv1.y - img->uv0.y);
    if (const UI::UISpriteAnimComponent* sa = w.GetComponent<UI::UISpriteAnimComponent>(e))
    {
        nw = tw / static_cast<float>((std::max)(1, sa->columns));
        nh = th / static_cast<float>((std::max)(1, sa->rows));
    }
    const std::type_index type(typeid(UI::UIRect));
    const std::string before = Blob(w, e, type);
    // A point axis takes the size; a stretched one the sizeDelta that resolves to it.
    // The pivot does not move (its point does not depend on the size).
    const auto c01 = [](float v) { return std::clamp(v, 0.f, 1.f); };
    const float s  = m_rowsScale > 0.f ? m_rowsScale : 1.f;
    const UI::Rect& p = r->parentRect;
    ur->size.x = nw - std::fabs(c01(ur->anchorMax.x) - c01(ur->anchorMin.x)) * (p.mx.x - p.mn.x) / s;
    ur->size.y = nh - std::fabs(c01(ur->anchorMax.y) - c01(ur->anchorMin.y)) * (p.mx.y - p.mn.y) / s;
    const std::string after = Blob(w, e, type);
    if (ctx.pushEdit && after != before)
        ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(w.MakeHandle(e), type, before, after, "UI Rect"));
    LOG_INFO("[UIEDITOR] native size %s -> %gx%g", w.GetName(e).c_str(), nw, nh);
    return true;
}

bool UIEditorWindow::WrapInRect(const UIEditorContext& ctx, Entity e, std::string& why)
{
    if (!ActiveTabPtr() || !ctx.world) { why = "no canvas open"; return false; }
    if (Frozen(ctx)) { why = FrozenWhy(ctx); return false; }
    if (!ctx.recordSpawn || !ctx.moveUINode || !ctx.pushEdit) { why = "this editor cannot restructure UI"; return false; }
    RebuildRows(ctx);
    const Row* r = FindRow(e);
    if (!r) { why = "not a node of the open canvas"; return false; }
    if (r->isRoot || !r->isRect) { why = "the canvas cannot be wrapped"; return false; }
    if (InstanceNode(ctx, e, &why)) return false;
    World& w = *ctx.world;
    const Entity parent = r->parent;
    std::vector<Entity> sibs;
    UI::UIChildrenOf(w, parent, sibs);
    const int at = static_cast<int>(std::find(sibs.begin(), sibs.end(), e) - sibs.begin());
    const UI::UIRect rect = *w.GetComponent<UI::UIRect>(e);
    const std::string name = w.GetName(e);
    if (ctx.beginMacro) ctx.beginMacro("Wrap " + name + " in Empty Rect");
    // The wrapper takes the node's place and rect; the node then fills it - the
    // picture does not change.
    Entity wrap = NullEntity;
    ctx.recordSpawn("UI Empty Rect", [&]
    {
        wrap = UITemplates::Create(w, UITemplates::Widget::EmptyRect, parent);
        if (wrap == NullEntity) return;
        UI::UIRect* wr = w.GetComponent<UI::UIRect>(wrap);
        const int sib = wr->siblingIndex;
        *wr = rect;
        wr->siblingIndex = sib;
        w.SetName(wrap, name + " Wrap");
    });
    bool ok = wrap != NullEntity && ctx.moveUINode(wrap, parent, at, &why) && ctx.moveUINode(e, wrap, 0, &why);
    if (ok)
    {
        const std::type_index type(typeid(UI::UIRect));
        const std::string before = Blob(w, e, type);
        UI::UIRect* er = w.GetComponent<UI::UIRect>(e);
        er->anchorMin = { 0.f, 0.f };
        er->anchorMax = { 1.f, 1.f };
        er->pivot     = { 0.5f, 0.5f };
        er->size      = { 0.f, 0.f };
        er->offset    = { 0.f, 0.f };
        er->visible   = true;   // the wrapper carries the node's visibility now
        const std::string after = Blob(w, e, type);
        if (after != before)
            ctx.pushEdit(std::make_unique<Undo::ComponentEditAction>(w.MakeHandle(e), type, before, after, "UI Rect"));
    }
    if (ctx.endMacro) ctx.endMacro();
    if (!ok)
    {
        if (why.empty()) why = "the wrapper could not be made";
        return false;
    }
    SelectNode(ctx, wrap);
    LOG_INFO("[UIEDITOR] wrap %s in '%s'", name.c_str(), w.GetName(wrap).c_str());
    return true;
}

std::vector<UIEditorWindow::Issue> UIEditorWindow::Check(const UIEditorContext& ctx)
{
    std::vector<Issue> out;
    const Tab* t = ActiveTabPtr();
    if (!t || !ctx.world) return out;
    World& w = *ctx.world;
    RebuildRows(ctx);
    const std::vector<Row> rows = m_rows;
    auto name = [&](Entity e) { const std::string& n = w.GetName(e); return n.empty() ? "entity " + std::to_string(e) : n; };
    char buf[256];

    // A size that resolves to nothing at the tab's resolution.
    for (const Row& r : rows)
        if (!r.isRoot && !r.hidden && (r.rect.mx.x - r.rect.mn.x <= 0.f || r.rect.mx.y - r.rect.mn.y <= 0.f))
        {
            std::snprintf(buf, sizeof(buf), "resolves to %.1f x %.1f px at %g x %g", r.rect.mx.x - r.rect.mn.x,
                          r.rect.mx.y - r.rect.mn.y, t->designW, t->designH);
            out.push_back({ r.e, "size", buf });
        }

    // Off the screen at EVERY preset: the kernel lays the canvas out for each.
    static const UI::Vec2 kScreens[] = { { 1280.f, 720.f }, { 1920.f, 1080.f }, { 2560.f, 1440.f }, { 3840.f, 2160.f },
                                         { 1080.f, 1920.f }, { 2560.f, 1080.f }, { 1024.f, 768.f } };
    UIView::Desc d;
    if (BuildDesc(ctx, *t, d))
    {
        std::unordered_map<Entity, int> seen;
        int laidOut = 0;
        for (const UI::Vec2& scr : kScreens)
        {
            d.designSize = scr;
            std::vector<UI::LayoutNode> nodes;
            float scale = 1.f;
            std::string why;
            if (!UIViewService::LayoutDesc(d, w, nodes, scale, why)) continue;
            ++laidOut;
            for (const UI::LayoutNode& n : nodes)
            {
                const UI::Rect& rc = n.cr.rect;
                if ((std::min)(rc.mx.x, scr.x) > (std::max)(rc.mn.x, 0.f) &&
                    (std::min)(rc.mx.y, scr.y) > (std::max)(rc.mn.y, 0.f))
                    ++seen[n.e];
            }
        }
        // Culled is not a reason to skip: a node off the screen IS culled by it. The
        // test is the node's own rect against each screen (a clip parent does not count).
        if (laidOut > 0)
            for (const Row& r : rows)
                if (!r.isRoot && !r.hidden && seen[r.e] == 0 && r.rect.mx.x > r.rect.mn.x && r.rect.mx.y > r.rect.mn.y)
                    out.push_back({ r.e, "offscreen", "off the screen at every preset (720p .. 4K, portrait, 21:9, 4:3)" });
    }

    // Textures and fonts that are not there.
    for (const Row& r : rows)
    {
        if (const UI::UIImage* im = w.GetComponent<UI::UIImage>(r.e); im && !im->texturePath.empty())
        {
            const Resource::TextureFileInfo info = Resource::DescribeTextureFile(im->texturePath);
            if (!info.ok) out.push_back({ r.e, "texture", "texture '" + im->texturePath + "': " + info.error });
        }
        if (const UI::UIText* tx = w.GetComponent<UI::UIText>(r.e); tx && !tx->fontPath.empty())
        {
            // Where the loaders look: the mounted pak, else the disk (a worktree's
            // shared content included).
            const Resource::AssetFS& afs = Resource::AssetFS::Get();
            std::string onDisk = afs.ResolveOnDisk(tx->fontPath);
            if (onDisk.empty()) onDisk = tx->fontPath;
            std::error_code ec;
            if (!afs.HasInPak(tx->fontPath) && !std::filesystem::exists(Sys::U8Path(onDisk), ec))
                out.push_back({ r.e, "font", "font '" + tx->fontPath + "' is missing (the default font draws it)" });
        }
    }

    // Two children of one parent with one name: a script finding by name gets either.
    std::vector<std::pair<std::pair<Entity, std::string>, Entity>> named;
    for (const Row& r : rows)
        if (!r.isRoot) named.push_back({ { r.parent, w.GetName(r.e) }, r.e });
    std::sort(named.begin(), named.end());
    for (std::size_t i = 1; i < named.size(); ++i)
        if (named[i].first == named[i - 1].first && !named[i].first.second.empty())
            out.push_back({ named[i].second, "name",
                            "'" + named[i].first.second + "' is the name of another child of '" +
                                name(named[i].first.first) + "' too" });

    // UI nodes of the world under no canvas: a parent GUID that does not resolve, a
    // cycle - never laid out, never drawn.
    if (const auto* rp = w.GetPool<UI::UIRect>())
        for (const Entity e : rp->Entities())
        {
            if (!w.IsAlive(e) || UI::FindOwningCanvas(w, e) != NullEntity) continue;
            // An asset tab's stage of a row template has no canvas by design.
            if (w.HasComponent<PreviewStageTag>(e)) continue;
            const UI::UIParent* up = w.GetComponent<UI::UIParent>(e);
            if (!up) continue;   // a UI template root under no canvas (a stage) is not this
            std::vector<Entity> chain{ e };
            bool cycle = false;
            for (Entity p = UI::ResolveUIParent(w, e); p != NullEntity && !cycle; p = UI::ResolveUIParent(w, p))
            {
                cycle = HasIn(chain, p);
                chain.push_back(p);
                if (chain.size() > 4096) break;
            }
            out.push_back({ e, cycle ? "cycle" : "unreachable",
                            cycle ? "its UI parents form a cycle: never laid out"
                                  : "no canvas above it (its parent GUID does not resolve): never drawn" });
        }
    LOG_INFO("[UIEDITOR] check '%s': %zu issue(s)", t->name.c_str(), out.size());
    return out;
}

void UIEditorWindow::ScanTemplates()
{
    m_templates.clear();
    m_templatesScanned = true;
    namespace fs = std::filesystem;
    for (const char* root : { "engine/ui", "asset" })
    {
        std::error_code ec;
        if (!fs::is_directory(Sys::U8Path(root), ec)) continue;
        for (fs::recursive_directory_iterator it(Sys::U8Path(root), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = Lower(Sys::PathToUtf8(it->path().extension()));
            std::string p = Sys::PathToUtf8(it->path());
            std::replace(p.begin(), p.end(), '\\', '/');
            if (ext == ".ipfb")
            {
                if (!IsUIPrefabFile(it->path())) continue;
            }
            else if (ext == ".iui")
            {
                // A widget template, not a canvas (a canvas is a root of its own).
                const std::shared_ptr<const UI::UIAsset> a = UI::LoadUIAsset(p, nullptr);
                if (!a || !a->root.Block("CvRect")) continue;
            }
            else continue;
            m_templates.push_back(p);
        }
    }
    std::sort(m_templates.begin(), m_templates.end());
}

void UIEditorWindow::DrawPalette(const UIEditorContext& ctx, Tab& t)
{
    (void)t;
    ImGui::SeparatorText("Palette");
    const float h = std::clamp(ImGui::GetContentRegionAvail().y * 0.42f, 110.f, 340.f);
    ImGui::BeginChild("##uiepalette", ImVec2(0.f, h), ImGuiChildFlags_None);
    std::function<void()> act;
    ImGui::BeginDisabled(Frozen(ctx));
    for (int i = 0; i < static_cast<int>(UITemplates::Widget::Count); ++i)
    {
        const auto w = static_cast<UITemplates::Widget>(i);
        ImGui::PushID(i);
        ImGui::Selectable(UITemplates::Label(w), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\nDrag it onto the designer or a Hierarchy row; double-click: into the selection.",
                              UITemplates::Tooltip(w));
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                act = [this, &ctx, w]
                {
                    std::string why;
                    if (CreateWidget(ctx, UITemplates::Name(w), NullEntity, false, {}, why) == NullEntity)
                        LOG_WARNING("[UIEDITOR] create: %s", why.c_str());
                };
        }
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("UIE_WIDGET", &i, sizeof(i));
            ImGui::TextUnformatted(UITemplates::Label(w));
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("Templates");
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) m_templatesScanned = false;
    if (!m_templatesScanned) ScanTemplates();
    if (m_templates.empty()) ImGui::TextDisabled("(no UI .iui or .ipfb in engine/ui or asset)");
    for (std::size_t i = 0; i < m_templates.size(); ++i)
    {
        const std::string& path = m_templates[i];
        ImGui::PushID(static_cast<int>(1000 + i));
        // The file name with its extension: a row converted to .iui sits beside its .ipfb.
        const std::string label = BaseName(path);
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\nA detached copy: drag it onto the designer, or double-click.", path.c_str());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                act = [this, &ctx, path]
                {
                    std::string why;
                    if (CreateWidget(ctx, path, NullEntity, false, {}, why) == NullEntity)
                        LOG_WARNING("[UIEDITOR] create %s: %s", path.c_str(), why.c_str());
                };
        }
        if (ImGui::BeginDragDropSource())
        {
            const bool iui = path.size() > 4 && Lower(path.substr(path.size() - 4)) == ".iui";
            ImGui::SetDragDropPayload(iui ? "IUI_PATH" : "IPFB_PATH", path.c_str(), path.size() + 1);
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    if (act) act();
}

void UIEditorWindow::DrawHierarchy(const UIEditorContext& ctx, Tab& t)
{
    ImGui::SeparatorText("Hierarchy");
    ImGui::BeginChild("##uiehier", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
    World* w = ctx.world;
    if (!w || m_rows.empty())
    {
        ImGui::TextDisabled(t.parked ? "(set aside during Play)" : "(nothing laid out)");
        ImGui::EndChild();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float indent = 12.f, fh = ImGui::GetFrameHeight();
    const char* iEye   = EditorTheme::Icon(FontAwesome::kEye);
    const char* iEyeNo = EditorTheme::Icon(FontAwesome::kEyeSlash);
    const char* iLock  = EditorTheme::Icon(FontAwesome::kLock);
    const char* iOpen  = EditorTheme::Icon(FontAwesome::kLockOpen);
    // Actions run after the loop: most lay the rows out again.
    std::function<void()> act;
    auto toggle = [&](const char* id, const char* icon, const char* fallback, bool on, const char* tip) -> bool
    {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 150, 160, 160));
        const bool clicked = ImGui::Button((std::string(icon ? icon : fallback) + id).c_str(), ImVec2(fh, fh));
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
    int foldDepth = -1;
    for (std::size_t i = 0; i < m_rows.size(); ++i)
    {
        const Row& r = m_rows[i];
        if (foldDepth >= 0 && r.depth > foldDepth) continue;
        foldDepth = -1;
        const bool folded = r.hasKids && HasIn(t.folded, r.e);
        if (folded) foldDepth = r.depth;
        const Entity e = r.e;
        ImGui::PushID(static_cast<int>(e));
        // The designer-only toggles first, in fixed columns.
        if (!r.isRoot)
        {
            const bool hid = HasIn(t.designHidden, e), lk = HasIn(t.locked, e);
            if (toggle("##eye", hid ? iEyeNo : iEye, hid ? "-" : "o", hid,
                       hid ? "Hidden in the designer (not saved; the game still draws it)" : "Hide in the designer"))
                act = [this, &ctx, e, hid] { std::string why; SetDesignerFlag(ctx, e, false, !hid, why); };
            ImGui::SameLine(0.f, 1.f);
            if (toggle("##lock", lk ? iLock : iOpen, lk ? "L" : "u", lk,
                       lk ? "Locked: a click in the designer goes through it" : "Lock in the designer"))
                act = [this, &ctx, e, lk] { std::string why; SetDesignerFlag(ctx, e, true, !lk, why); };
        }
        else
            ImGui::Dummy(ImVec2(2.f * fh + 1.f, fh));
        ImGui::SameLine(0.f, 4.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(r.depth) * indent);
        // The fold arrow.
        if (r.hasKids)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            if (ImGui::ArrowButton("##fold", folded ? ImGuiDir_Right : ImGuiDir_Down))
                act = [&t, e, folded] { SetIn(t.folded, e, !folded); };
            ImGui::PopStyleColor();
        }
        else
            ImGui::Dummy(ImVec2(fh, fh));
        ImGui::SameLine(0.f, 2.f);
        std::string label = w->GetName(e);
        if (label.empty()) label = "(entity " + std::to_string(e) + ")";
        const bool dim = r.hidden || r.culled || r.skip;
        // A UI Document instance's node (the .iui's, read-only here) in its own colour.
        const Entity docHost = UI::DocumentHostOf(*w, e);
        const UI::UIDocument* hostDoc = w->GetComponent<UI::UIDocument>(docHost != NullEntity ? docHost : e);
        if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        else if (docHost != NullEntity) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 210, 230, 255));
        if (ImGui::Selectable((label + "##row").c_str(), e == Sel(ctx), ImGuiSelectableFlags_AllowDoubleClick))
        {
            if (e != Sel(ctx)) SelectNode(ctx, e);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && e == Sel(ctx)) FrameSelection(ctx, t);
        }
        if (dim || docHost != NullEntity) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && (r.hidden || r.culled))
            ImGui::SetTooltip(r.hidden ? "Not visible (its Visible is off, or an ancestor's)" : "Clipped away, or off its parent");
        else if (ImGui::IsItemHovered() && hostDoc)
            ImGui::SetTooltip(docHost != NullEntity ? "From %s (a UI Document instance): read-only - right-click > Edit the UI Asset"
                                                    : "UI Document: %s is instanced here", hostDoc->path.c_str());
        // Drag a row onto another: before it (top quarter), after it (bottom quarter), into it.
        if (!r.isRoot && !Frozen(ctx) && !InstanceNode(ctx, e) && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("UIE_NODE", &e, sizeof(e));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (!Frozen(ctx) && ImGui::BeginDragDropTarget())
        {
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            const float  fy = (io.MousePos.y - mn.y) / (std::max)(1.f, mx.y - mn.y);
            const int    zone = r.isRoot ? 0 : (fy < 0.25f ? -1 : (fy > 0.75f ? 1 : 0));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 c = IM_COL32(255, 200, 60, 255);
            if (zone == 0) dl->AddRect(mn, mx, c, 0.f, 0, 1.5f);
            else dl->AddLine(ImVec2(mn.x, zone < 0 ? mn.y : mx.y), ImVec2(mx.x, zone < 0 ? mn.y : mx.y), c, 2.f);
            const Entity target = e, targetParent = r.parent;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("UIE_NODE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                Entity child = NullEntity;
                std::memcpy(&child, p->Data, sizeof(child));
                act = [this, &ctx, child, target, targetParent, zone]
                {
                    std::string why;
                    bool ok = false;
                    if (child == target) return;
                    // A row dragged from ANOTHER tab (a drag survives a tab switch)
                    // is not this tab's to move: a scene node into a stage or back
                    // would land in the wrong undo history (MoveUINode refuses too).
                    if (!FindRow(child))
                    {
                        LOG_WARNING("[UIEDITOR] move: entity %u is not a node of this tab", static_cast<unsigned>(child));
                        return;
                    }
                    if (zone == 0) ok = ctx.moveUINode && ctx.moveUINode(child, target, -1, &why);
                    else
                    {
                        // Beside the row: its parent, at its place (counted without the child).
                        std::vector<Entity> sibs;
                        UI::UIChildrenOf(*ctx.world, targetParent, sibs);
                        sibs.erase(std::remove(sibs.begin(), sibs.end(), child), sibs.end());
                        int at = static_cast<int>(std::find(sibs.begin(), sibs.end(), target) - sibs.begin());
                        if (zone > 0) ++at;
                        ok = ctx.moveUINode && ctx.moveUINode(child, targetParent, at, &why);
                    }
                    if (!ok) LOG_WARNING("[UIEDITOR] move: %s", why.c_str());
                };
            }
            else if (const ImGuiPayload* q = ImGui::AcceptDragDropPayload("UIE_WIDGET"))
            {
                int wi = 0;
                std::memcpy(&wi, q->Data, sizeof(wi));
                const Entity into = (zone == 0) ? target : targetParent;
                act = [this, &ctx, wi, into]
                {
                    std::string why;
                    if (CreateWidget(ctx, UITemplates::Name(static_cast<UITemplates::Widget>(wi)), into, false, {}, why) ==
                        NullEntity)
                        LOG_WARNING("[UIEDITOR] create: %s", why.c_str());
                };
            }
            else if (const ImGuiPayload* x = ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const std::string path(static_cast<const char*>(x->Data));
                const Entity into = (zone == 0) ? target : targetParent;
                act = [this, &ctx, path, into]
                {
                    std::string why;
                    if (CreateWidget(ctx, path, into, false, {}, why) == NullEntity)
                        LOG_WARNING("[UIEDITOR] create %s: %s", path.c_str(), why.c_str());
                };
            }
            else if (const ImGuiPayload* u = ImGui::AcceptDragDropPayload("IUI_PATH"))   // a .iui
            {
                const std::string path(static_cast<const char*>(u->Data));
                const Entity into = (zone == 0) ? target : targetParent;
                act = [this, &ctx, path, into]
                {
                    std::string why;
                    if (CreateWidget(ctx, path, into, false, {}, why) == NullEntity)
                        LOG_WARNING("[UIEDITOR] create %s: %s", path.c_str(), why.c_str());
                };
            }
            ImGui::EndDragDropTarget();
        }
        // The row's menu.
        if (ImGui::BeginPopupContextItem("##ctx"))
        {
            // A UI Document instance's node, or its host, opens the asset; nothing
            // else changes an instance's node.
            if (hostDoc && !hostDoc->path.empty())
            {
                if (ImGui::MenuItem("Edit the UI Asset")) RequestOpenAsset(hostDoc->path);
                ImGui::Separator();
            }
            ImGui::BeginDisabled(Frozen(ctx) || docHost != NullEntity);
            if (ImGui::BeginMenu("Create Child"))
            {
                for (int k = 0; k < static_cast<int>(UITemplates::Widget::Count); ++k)
                    if (ImGui::MenuItem(UITemplates::Label(static_cast<UITemplates::Widget>(k))))
                        act = [this, &ctx, k, e]
                        {
                            std::string why;
                            if (CreateWidget(ctx, UITemplates::Name(static_cast<UITemplates::Widget>(k)), e, false, {},
                                             why) == NullEntity)
                                LOG_WARNING("[UIEDITOR] create: %s", why.c_str());
                        };
                ImGui::EndMenu();
            }
            if (!r.isRoot)
            {
                if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                    act = [this, &ctx, e]
                    {
                        std::string why;
                        if (Duplicate(ctx, e, why) == NullEntity) LOG_WARNING("[UIEDITOR] duplicate: %s", why.c_str());
                    };
                if (ImGui::MenuItem("Wrap in Empty Rect"))
                    act = [this, &ctx, e]
                    {
                        std::string why;
                        if (!WrapInRect(ctx, e, why)) LOG_WARNING("[UIEDITOR] wrap: %s", why.c_str());
                    };
                if (ImGui::MenuItem("Rename", "F2"))
                {
                    m_renaming = e;
                    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", w->GetName(e).c_str());
                    m_renameFocus = true;
                }
                if (const UI::UIText* tx = w->GetComponent<UI::UIText>(e); tx && ImGui::MenuItem("Edit Text"))
                {
                    m_textEdit = e;
                    m_textBuf.assign(tx->text.begin(), tx->text.end());
                    m_textBuf.push_back('\0');
                    m_textBuf.resize((std::max)(m_textBuf.size(), static_cast<std::size_t>(256)));
                    m_textFocus = true;
                    m_textAt[0] = io.MousePos.x;
                    m_textAt[1] = io.MousePos.y;
                }
                if (w->HasComponent<UI::UIImage>(e) && ImGui::MenuItem("Native Size"))
                    act = [this, &ctx, e]
                    {
                        std::string why;
                        if (!NativeSize(ctx, e, why)) LOG_WARNING("[UIEDITOR] native size: %s", why.c_str());
                    };
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del") && ctx.deleteEntity) act = [&ctx, e] { ctx.deleteEntity(e); };
            }
            ImGui::EndDisabled();
            // This node and its subtree as a .iui of its own (it reads; a template
            // and Play allow it too), then opened in a tab.
            ImGui::Separator();
            if (ImGui::MenuItem("Save Branch as UI Asset..."))
            {
                const std::string path = SaveFileDialog(kIuiFilter, "iui", "asset");
                if (!path.empty())
                    act = [this, &ctx, e, path]
                    {
                        std::string why;
                        if (!SaveBranch(ctx, e, path, /*overwrite*/ true, /*open*/ true, why))
                            LOG_WARNING("[UIEDITOR] save branch: %s", why.c_str());
                    };
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write this node and everything under it as a .iui UI asset (a canvas: a canvas asset)\n"
                                  "and open it in a tab. The scene is not changed.");
            // The branch becomes a .iui AND the scene references it (a UI Document).
            if (t.kind == TabKind::Scene && docHost == NullEntity)
            {
                ImGui::BeginDisabled(Frozen(ctx));
                if (ImGui::MenuItem("Extract to UI Asset..."))
                {
                    const std::string path = SaveFileDialog(kIuiFilter, "iui", "asset");
                    if (!path.empty())
                        act = [this, &ctx, e, path]
                        {
                            std::string why;
                            if (Extract(ctx, e, path, /*overwrite*/ true, why) == NullEntity)
                                LOG_WARNING("[UIEDITOR] extract: %s", why.c_str());
                        };
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Write this node and everything under it as a .iui, and put a UI Document in its\n"
                                      "place that references it: the scene keeps the reference, the file the content.\n"
                                      "The picture does not change; one undo step.");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (act) act();
}

void UIEditorWindow::DrawRectDetails(const UIEditorContext& ctx, const Row& r)
{
    if (!ImGui::CollapsingHeader("UI Rect", ImGuiTreeNodeFlags_DefaultOpen)) return;
    UI::UIRect* ur = ctx.world ? ctx.world->GetComponent<UI::UIRect>(r.e) : nullptr;
    if (!ur) return;
    if (ImGui::Button("Anchors...")) ImGui::OpenPopup("##uieanchors");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The 4x4 presets - the rect stays where it is.\nAlt: the pivot too. Shift: the position too.");
    DrawAnchorPresetPopup(ctx);
    ImGui::SameLine();
    ImGui::Checkbox("Keep visual", &m_keepVisual);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: editing the anchors or the pivot re-solves offset / size so the rect does not move (E14).\n"
                          "Off: the raw values - the rect jumps.");
    const Entity e = r.e;
    UIRectEdit::Ctx rc;
    rc.parent    = r.parentRect;
    rc.hasParent = true;
    rc.scale     = m_rowsScale;
    const bool keep = m_keepVisual;
    if (ctx.drawWithUndo)
        ctx.drawWithUndo(e, std::type_index(typeid(UI::UIRect)), [&]
        {
            UI::Vec2 aMin = ur->anchorMin, aMax = ur->anchorMax, piv = ur->pivot;
            bool anchors = ImGui::DragFloat2("Anchor Min", &aMin.x, 0.005f, 0.f, 1.f, "%.3f");
            anchors |= ImGui::DragFloat2("Anchor Max", &aMax.x, 0.005f, 0.f, 1.f, "%.3f");
            if (anchors)
            {
                if (keep) *ur = UIRectEdit::SetAnchorsKeepRect(*ur, aMin, aMax, UIRectEdit::Resolve(*ur, rc), rc);
                else { ur->anchorMin = aMin; ur->anchorMax = aMax; }
            }
            if (ImGui::DragFloat2("Pivot", &piv.x, 0.005f, 0.f, 1.f, "%.3f"))
            {
                if (keep) *ur = UIRectEdit::SetPivotKeepRect(*ur, piv, UIRectEdit::Resolve(*ur, rc), rc);
                else ur->pivot = piv;
            }
            ImGui::DragFloat2("Size (ref px)", &ur->size.x, 0.5f, 0.f, 0.f, "%.1f");
            ImGui::DragFloat2("Offset (ref px)", &ur->offset.x, 0.5f, 0.f, 0.f, "%.1f");
            ImGui::Checkbox("Visible (subtree)", &ur->visible);
        });
    ImGui::TextDisabled("Paint order %d", ur->siblingIndex);
    ImGui::SameLine();
    // Outside the bracket: a move is its own undo step.
    if (ImGui::SmallButton("Earlier"))
    {
        std::string why;
        Reorder(ctx, e, -1, why);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Later"))
    {
        std::string why;
        Reorder(ctx, e, 1, why);
    }
    const UI::Rect& rr = r.rect;
    ImGui::TextDisabled("Design rect %.1f, %.1f  %.1f x %.1f  (scale %.4f)", rr.mn.x, rr.mn.y, rr.mx.x - rr.mn.x,
                        rr.mx.y - rr.mn.y, m_rowsScale);
}

void UIEditorWindow::DrawDetails(const UIEditorContext& ctx, Tab& t)
{
    const Row* found = FindRow(Sel(ctx));
    if (!found || !ctx.world)
    {
        ImGui::TextDisabled("Select a node: click it in the designer or pick it in the Hierarchy.");
        return;
    }
    const Row r = *found;   // the rows may be laid out again below (Earlier / Later, Native Size)
    World& w = *ctx.world;
    const Entity e = r.e;
    // A UI Document instance's node is the .iui's: read-only here, its asset a click away.
    std::string instanceWhy;
    const bool instance = InstanceNode(ctx, e, &instanceWhy);
    if (instance)
    {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "READ-ONLY");
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit the UI Asset"))
            if (const UI::UIDocument* d = w.GetComponent<UI::UIDocument>(UI::DocumentHostOf(w, e)))
                RequestOpenAsset(d->path);
        ImGui::TextWrapped("%s", instanceWhy.c_str());
    }
    ImGui::BeginDisabled(Frozen(ctx) || instance);
    if (Frozen(ctx))
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "%s",
                           ctx.playing ? "PLAYING - read-only until Stop" : FrozenWhy(ctx).c_str());
    char name[256];
    std::snprintf(name, sizeof(name), "%s", w.GetName(e).c_str());
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##uiename", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && ctx.rename)
        ctx.rename(e, name);
    ImGui::TextDisabled("entity %u%s", static_cast<unsigned>(e), r.isRoot ? "  -  the canvas" : "");
    if (r.isRoot)
    {
        if (ctx.drawComponent) ctx.drawComponent(e, std::type_index(typeid(UI::UICanvas)));
        ImGui::TextDisabled("At %g x %g the canvas scale is %.4f.", t.designW, t.designH, m_rowsScale);
    }
    else if (r.isRect)
        DrawRectDetails(ctx, r);
    // The rest of its UI components, with the Inspector's own editors. A UI Document
    // (a .iui instanced under the node) is a scene's: a .iui file cannot hold one, so
    // an asset tab does not offer it.
    static const std::type_index kTypes[] = {
        std::type_index(typeid(UI::UIImage)), std::type_index(typeid(UI::UIText)),
        std::type_index(typeid(UI::UIInteractable)), std::type_index(typeid(UI::UIClipRect)),
        std::type_index(typeid(UI::UISpriteAnimComponent)), std::type_index(typeid(UI::UIDocument)) };
    static const char* kLabels[] = { "Canvas Image", "Canvas Text", "UI Interactable", "UI Clip Rect", "Sprite Animation",
                                     "UI Document" };
    const bool has[] = { w.HasComponent<UI::UIImage>(e), w.HasComponent<UI::UIText>(e),
                         w.HasComponent<UI::UIInteractable>(e), w.HasComponent<UI::UIClipRect>(e),
                         w.HasComponent<UI::UISpriteAnimComponent>(e), w.HasComponent<UI::UIDocument>(e) };
    const int kinds = t.kind == TabKind::Scene ? 6 : 5;
    for (int i = 0; i < 6; ++i)
        if (has[i] && ctx.drawComponent) ctx.drawComponent(e, kTypes[i]);
    if (has[0] && !r.isRoot)
    {
        if (ImGui::Button("Native Size"))
        {
            std::string why;
            if (!NativeSize(ctx, e, why)) LOG_WARNING("[UIEDITOR] native size: %s", why.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Size the node to its texture: one reference pixel a texel (a sprite sheet: one frame).");
    }
    if (!r.isRoot && ctx.addComponent)
    {
        ImGui::Separator();
        if (ImGui::Button("+ Add UI Component")) ImGui::OpenPopup("##uieadd");
        if (ImGui::BeginPopup("##uieadd"))
        {
            bool any = false;
            for (int i = 0; i < kinds; ++i)
                if (!has[i])
                {
                    any = true;
                    if (ImGui::MenuItem(kLabels[i])) ctx.addComponent(e, kTypes[i]);
                }
            if (!any) ImGui::TextDisabled("(it has every UI component)");
            ImGui::EndPopup();
        }
    }
    ImGui::EndDisabled();
}

void UIEditorWindow::DrawCheck(const UIEditorContext& ctx)
{
    const Tab* t = ActiveTabPtr();
    if (!t) return;
    if (t->subject != m_issuesSubject) m_issuesRun = false;   // another canvas: its own list
    if (!m_issuesRun || ImGui::Button("Check again"))
    {
        m_issues        = Check(ctx);
        m_issuesRun     = true;
        m_issuesSubject = t->subject;
    }
    if (m_issues.empty())
    {
        ImGui::TextDisabled("Nothing found.");
        return;
    }
    ImGui::TextDisabled("Click an issue to select its node.");
    for (std::size_t i = 0; i < m_issues.size(); ++i)
    {
        const Issue& is = m_issues[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string who = ctx.world && ctx.world->IsAlive(is.e) ? ctx.world->GetName(is.e) : std::string("(gone)");
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f), "%s", is.kind.c_str());
        ImGui::SameLine();
        if (ImGui::Selectable((who + "##iss").c_str(), is.e == Sel(ctx)) && ctx.world &&
            ctx.world->IsAlive(is.e))
            SelectNode(ctx, is.e);
        ImGui::Indent();
        ImGui::TextWrapped("%s", is.text.c_str());
        ImGui::Unindent();
        ImGui::PopID();
    }
}

void UIEditorWindow::DrawTextEditor(const UIEditorContext& ctx)
{
    if (m_textEdit == NullEntity) return;
    if (!ctx.world || !ctx.world->IsAlive(m_textEdit) || Frozen(ctx))
    {
        m_textEdit = NullEntity;
        return;
    }
    if (m_textFocus)
    {
        ImGui::OpenPopup("##uietext");
        ImGui::SetNextWindowPos(ImVec2(m_textAt[0], m_textAt[1]));
    }
    if (!ImGui::BeginPopup("##uietext"))
    {
        m_textEdit = NullEntity;   // clicked away: cancelled
        return;
    }
    if (m_textFocus)
    {
        ImGui::SetKeyboardFocusHere();
        m_textFocus = false;
    }
    if (m_textBuf.empty()) m_textBuf.push_back('\0');
    const bool enter = ImGui::InputTextMultiline("##text", m_textBuf.data(), m_textBuf.size(), ImVec2(380.f, 96.f),
                                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                                     ImGuiInputTextFlags_CtrlEnterForNewLine |
                                                     ImGuiInputTextFlags_CallbackResize,
                                                 TextBufResize, &m_textBuf);
    ImGui::TextDisabled("Enter applies   Ctrl+Enter: new line   Esc cancels");
    if (enter)
    {
        std::string why;
        if (!SetText(ctx, m_textEdit, std::string(m_textBuf.data()), why))
            LOG_WARNING("[UIEDITOR] text: %s", why.c_str());
        m_textEdit = NullEntity;
        ImGui::CloseCurrentPopup();
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_textEdit = NullEntity;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ==== Harness ====
bool UIEditorWindow::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    static const char* kFlags[] = {
        "-uieditor_size", "-uieditor_view", "-uieditor_res", "-uieditor_zoom", "-uieditor_pan", "-uieditor_bg",
        "-uieditor_shot", "-uieditor_close_at", "-uieditor_parity_skew", "-uieditor_select", "-uieditor_drag",
        "-uieditor_runrect_off", "-uieditor_idle_record", "-uieditor_write_computed", "-uieditor_anchor",
        "-uieditor_anchor_raw",
    };
    // The stage isolation pack's reverse arm: read whether or not a tab opens by
    // flag (that pack opens its asset from the console).
    m_harnessOffscreenOff = cl.Has("-uieditor_offscreen_off");
    const bool hasOpen = cl.Has("-uieditor_open");
    for (const char* f : kFlags)
        if (cl.Has(f) && !hasOpen)
        {
            why = std::string(f) + " without -uieditor_open=<canvas name> has no subject";
            return false;
        }
    if (!hasOpen) return true;
    m_harnessOpen = Resource::AssetRefs::PercentDecode(cl.Get("-uieditor_open"));
    if (m_harnessOpen.empty())
    {
        why = "-uieditor_open needs the name of a UICanvas entity, or a .iui / .ipfb path";
        return false;
    }
    // A file opens as an asset / template tab (no waiting for a canvas).
    m_harnessOpenAsset = EndsWithCI(m_harnessOpen, ".iui") || EndsWithCI(m_harnessOpen, ".ipfb");
    m_harnessParitySkew = cl.Has("-uieditor_parity_skew");
    // The packs' reverse arms (UIView::kDebug*): uieditor_zoom_text, uieditor_idle_cost,
    // uieditor_resolution each go red with theirs.
    if (cl.Has("-uieditor_runrect_off"))    m_harnessDebug |= UIView::kDebugRunRectOff;
    if (cl.Has("-uieditor_idle_record"))    m_harnessDebug |= UIView::kDebugIdleRecord;
    if (cl.Has("-uieditor_write_computed")) m_harnessDebug |= UIView::kDebugWriteComputed;
    m_harnessAnchorRaw = cl.Has("-uieditor_anchor_raw");   // uieditor_anchor's reverse arm
    if (cl.Has("-uieditor_size"))
    {
        std::uint32_t w = 0, h = 0;
        if (!ParseSize(cl.Get("-uieditor_size"), w, h) || w < 16 || h < 16 || w > UIViewService::kMaxSize ||
            h > UIViewService::kMaxSize)
        {
            why = "-uieditor_size needs <w>x<h> (16..4096)";
            return false;
        }
        PinSize(w, h);
    }
    if (cl.Has("-uieditor_bg"))
    {
        m_harnessBg = cl.Get("-uieditor_bg");
        if (m_harnessBg.empty()) { why = "-uieditor_bg needs checker, none, solid or solid:r,g,b"; return false; }
    }
    // <value>@<frame> lists; each becomes a view step "key=value" (or a spec).
    auto list = [&](const char* flag, int kind, const char* key) -> bool
    {
        if (!cl.Has(flag)) return true;
        const std::string v = cl.Get(flag);
        if (v.empty()) { why = std::string(flag) + " needs a value"; return false; }
        for (const std::string& item : SplitList(v, ','))
        {
            Step s;
            if (!SplitAt(item, s.text, s.at)) { why = std::string(flag) + ": '" + item + "' is not <value>@<frame>"; return false; }
            s.kind = kind;
            if (kind == kStepShot) s.text = HarnessPath(s.text);
            else if (key) s.text = std::string(key) + "=" + s.text;
            m_steps.push_back(std::move(s));
        }
        return true;
    };
    if (!list("-uieditor_view", kStepView, nullptr) || !list("-uieditor_res", kStepView, "res") ||
        !list("-uieditor_zoom", kStepView, "zoom") || !list("-uieditor_pan", kStepView, "pan") ||
        !list("-uieditor_shot", kStepShot, nullptr) || !list("-uieditor_select", kStepSelect, nullptr) ||
        !list("-uieditor_drag", kStepDrag, nullptr) || !list("-uieditor_anchor", kStepAnchor, nullptr))
        return false;
    for (const Step& s : m_steps)
        if (s.kind == kStepAnchor)
        {
            const std::vector<std::string> p = SplitList(s.text, ':');
            bool good = !p.empty() && IsAnchorPreset(p[0]);
            for (std::size_t i = 1; good && i < p.size(); ++i) good = p[i] == "pivot" || p[i] == "pos" || p[i] == "raw";
            if (!good)
            {
                why = "-uieditor_anchor: '" + s.text + "' is not <top_left..stretch_full>[:pivot][:pos][:raw]";
                return false;
            }
        }
    // A drag is <handle>:<dx>:<dy>[:ctrl][:shift][:alt] (design px).
    for (const Step& s : m_steps)
        if (s.kind == kStepDrag)
        {
            const std::vector<std::string> p = SplitList(s.text, ':');
            float f = 0.f;
            bool good = p.size() >= 3 && HandleFromName(p[0]) >= 0 && ParseFloat(p[1], f) && ParseFloat(p[2], f);
            for (std::size_t i = 3; good && i < p.size(); ++i)
                good = p[i] == "ctrl" || p[i] == "shift" || p[i] == "alt";
            if (!good)
            {
                why = "-uieditor_drag: '" + s.text + "' is not <body|tl|tr|br|bl|t|r|b|l>:<dx>:<dy>[:ctrl][:shift][:alt]";
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
                    why = "-uieditor_view: '" + kv + "' is not key=value (keys res, zoom, pan, bg, state, hidden, "
                          "realtime, size; ';' between them)";
                    return false;
                }
    if (cl.Has("-uieditor_close_at"))
    {
        std::uint32_t f = 0;
        if (!ParseUInt(cl.Get("-uieditor_close_at"), f)) { why = "-uieditor_close_at needs an editor frame"; return false; }
        Step s;
        s.kind = kStepClose;
        s.at   = f;
        m_steps.push_back(std::move(s));
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    LOG_INFO("[UIEDITOR] harness: open='%s' steps=%zu size=%ux%u bg='%s' paritySkew=%d debug=%u", m_harnessOpen.c_str(),
             m_steps.size(), m_pinW, m_pinH, m_harnessBg.c_str(), m_harnessParitySkew ? 1 : 0,
             static_cast<unsigned>(m_harnessDebug));
    return true;
}

void UIEditorWindow::TickHarness(const UIEditorContext& ctx)
{
    if (m_harnessOpen.empty() || m_harnessFailed || !ctx.world) return;
    if (!m_harnessOpened)
    {
        std::string why;
        if (m_harnessOpenAsset)
        {
            // A .iui / .ipfb opens in a tab of its own.
            m_harnessOpened = true;
            if (!OpenAssetNow(ctx, m_harnessOpen, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_open='%s' cannot open the UI Editor: %s", m_harnessOpen.c_str(), why.c_str());
                return;
            }
        }
        else
        {
            // The canvas may come from the scene's script a few frames in.
            const Entity e = FindCanvasByName(*ctx.world, m_harnessOpen);
            if (e == NullEntity)
            {
                if (ctx.editorFrame > kOpenDeadline)
                {
                    m_harnessFailed = true;
                    LOG_ERROR("-uieditor_open='%s': no UICanvas entity of that name by editor frame %llu",
                              m_harnessOpen.c_str(), static_cast<unsigned long long>(ctx.editorFrame));
                }
                return;
            }
            m_harnessOpened = true;
            if (!OpenNow(ctx, e, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_open='%s' cannot open the UI Editor: %s", m_harnessOpen.c_str(), why.c_str());
                return;
            }
        }
        if (!m_harnessBg.empty() && !Set("bg", m_harnessBg, why))
        {
            m_harnessFailed = true;
            LOG_ERROR("-uieditor_bg: %s", why.c_str());
            return;
        }
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
                LOG_ERROR("-uieditor view step '%s' at editor frame %llu: %s", s.text.c_str(),
                          static_cast<unsigned long long>(s.at), why.c_str());
            }
            else
                LOG_INFO("[UIEDITOR] harness view -> %s (frame %llu)", s.text.c_str(),
                         static_cast<unsigned long long>(ctx.editorFrame));
            s.done = true;
            // The new description reaches the view in THIS frame's PPA below: a
            // shot on the same frame waits for its picture.
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
                    LOG_ERROR("[UIEDITOR] shot '%s' at editor frame %llu: the picture never settled", s.text.c_str(),
                              static_cast<unsigned long long>(s.at));
                }
                return;   // steps run in order
            }
            std::string why;
            if (!WriteShot(ctx, s.text, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_shot '%s' failed: %s", s.text.c_str(), why.c_str());
            }
            s.done = true;
            break;
        }
        case kStepClose:
            CloseAll(ctx);
            m_open = false;
            s.done = true;
            break;
        case kStepSelect:
        {
            // A node of the open canvas by name ("none" clears). The selection
            // reaches the context next frame: one step a frame.
            std::string why;
            Entity e = NullEntity;
            if (Lower(s.text) != "none")
            {
                RebuildRows(ctx);
                int named = 0;
                for (const Row& r : m_rows)
                    if (ctx.world->GetName(r.e) == s.text && named++ == 0) e = r.e;
                if (e == NullEntity) why = "no node of the open canvas is named '" + s.text + "'";
                // Never the first of several: which one is the paint order's accident.
                else if (named > 1) why = std::to_string(named) + " nodes of the open canvas are named '" + s.text + "'";
            }
            if (!why.empty() || !Select(ctx, e, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_select '%s': %s", s.text.c_str(), why.c_str());
            }
            else
                LOG_INFO("[UIEDITOR] harness select -> %s (frame %llu)", s.text.c_str(),
                         static_cast<unsigned long long>(ctx.editorFrame));
            s.done = true;
            return;
        }
        case kStepAnchor:
        {
            const std::vector<std::string> p = SplitList(s.text, ':');
            bool pivot = false, pos = false, raw = false;
            for (std::size_t i = 1; i < p.size(); ++i) (p[i] == "pivot" ? pivot : p[i] == "pos" ? pos : raw) = true;
            std::string why;
            if (!ApplyAnchorPreset(ctx, p[0], pivot, pos, !raw, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_anchor '%s': %s", s.text.c_str(), why.c_str());
            }
            s.done = true;
            return;
        }
        case kStepDrag:
        {
            const std::vector<std::string> p = SplitList(s.text, ':');
            float dx = 0.f, dy = 0.f;
            ParseFloat(p[1], dx);
            ParseFloat(p[2], dy);
            bool ctrl = false, shift = false, alt = false;
            for (std::size_t i = 3; i < p.size(); ++i)
                (p[i] == "ctrl" ? ctrl : p[i] == "shift" ? shift : alt) = true;
            std::string why;
            if (!Drag(ctx, HandleFromName(p[0]), UI::Vec2{ dx, dy }, ctrl, shift, alt, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-uieditor_drag '%s': %s", s.text.c_str(), why.c_str());
            }
            s.done = true;
            return;
        }
        default:
            s.done = true;
            break;
        }
    }
}

} // namespace Editor
