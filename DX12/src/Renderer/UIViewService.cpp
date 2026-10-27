// UIViewService.cpp - off-screen UI views for the designer; the contract is in the header.
#include "Renderer/UIViewService.h"

#include "ECS/ECS.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "System/Log.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace
{
    // Backdrop tile: the checker is ONE textured quad over the design frame,
    // tiled with a wrap + point sampler (a quad per cell would spend thousands of
    // the draw list's 65535 indices on the backdrop alone).
    constexpr float kCheckerCellPx = 16.f;   // design pixels per checker cell

    uint64_t Mix(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
}

UIViewService::UIViewService(IGraphicsDevice& gfx)
    : m_gfx(gfx)
{
    m_views.resize(kMaxViews);
}

UIViewService::~UIViewService() = default;

// ==== Main thread ====
std::uint32_t UIViewService::Acquire()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (std::uint32_t i = 0; i < m_views.size(); ++i)
    {
        View& v = m_views[i];
        if (v.live || v.releasePending) continue;
        v.live       = true;
        v.hasRequest = false;
        return i;
    }
    LOG_WARNING("[UIVIEW] all %u UI views are taken - the picture is not drawn", kMaxViews);
    return kInvalidId;
}

void UIViewService::Request(std::uint32_t id, const UIView::Desc& desc)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    View& v = m_views[id];
    if (!v.live || v.releasePending) return;
    v.requested  = desc;
    v.hasRequest = true;
}

void UIViewService::Release(std::uint32_t id)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    View& v = m_views[id];
    if (v.live) v.releasePending = true;
}

std::uint64_t UIViewService::Srv(std::uint32_t id) const
{
    if (id >= m_views.size()) return 0;
    const View& v = m_views[id];
    return v.target.IsValid() ? m_gfx.GetTextureSRVGpuHandle(v.target) : 0;
}

const RHI::Texture* UIViewService::Output(std::uint32_t id) const
{
    if (id >= m_views.size()) return nullptr;
    const View& v = m_views[id];
    return v.target.IsValid() ? &v.target : nullptr;
}

UIView::Status UIViewService::GetStatus(std::uint32_t id) const
{
    UIView::Status s;
    if (id >= m_views.size()) return s;
    const View& v = m_views[id];
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        s.live = v.live && !v.releasePending;
    }
    std::lock_guard<std::mutex> lk(m_statusMutex);
    const bool live = s.live;
    s = v.paintStatus;
    s.live            = live;
    s.width           = v.target.IsValid() ? v.target.desc.width : 0;
    s.height          = v.target.IsValid() ? v.target.desc.height : 0;
    s.renders         = v.renders;
    s.lastRenderFrame = v.lastRenderFrame;
    s.hasOutput       = v.target.IsValid() && v.renders > 0;
    // paintedHash / hasDrawList are the game half's, written on this (main)
    // thread; recordedHash is the render half's, under the lock held here.
    s.current         = s.hasOutput && v.hasDrawList && v.recordedHash != 0 && v.recordedHash == v.paintedHash;
    return s;
}

std::uint32_t UIViewService::LiveCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::uint32_t n = 0;
    for (const View& v : m_views) n += (v.live && !v.releasePending) ? 1u : 0u;
    return n;
}

std::uint64_t UIViewService::TargetBytes() const
{
    std::uint64_t n = 0;
    for (const View& v : m_views) n += v.target.IsValid() ? v.targetBytes : 0;
    return n;
}

UI::Vec2 UIViewService::MainScreen() const
{
    std::lock_guard<std::mutex> lk(m_statusMutex);
    return m_mainScreen;
}

// ==== Layout ====
bool UIViewService::LayoutDesc(const UIView::Desc& d, World& world, std::vector<UI::LayoutNode>& out,
                               float& canvasScale, std::string& why)
{
    out.clear();
    canvasScale = 1.f;
    if (d.subject == NullEntity || !world.IsAlive(d.subject) || world.Epoch() != d.worldEpoch)
    {
        why = "the subject is gone (deleted, or the world was reloaded)";
        return false;
    }
    if (d.designSize.x < 1.f || d.designSize.y < 1.f)
    {
        why = "no design size";
        return false;
    }

    // Child lists over the whole world, like the game screen's: a subject's edges are
    // the same UIParent pool. Resolving a parentGuid writes UIParent.parent - the value
    // the game screen's own resolve writes, so this is idempotent.
    static thread_local UI::ChildLists t_lists;
    UI::BuildChildLists(world, t_lists);

    const UI::Rect screen{ UI::Vec2{ 0.f, 0.f }, d.designSize };
    UI::LayoutRoot root;
    root.root             = d.subject;
    root.rootClip         = screen;
    root.ignoreVisibility = d.showHidden;
    if (const UI::UICanvas* cv = world.GetComponent<UI::UICanvas>(d.subject))
    {
        root.rootIsCanvas = true;
        root.rootRect     = screen;
        root.scale        = UI::ComputeCanvasScale(*cv, d.designSize);
        root.sortBase     = cv->sortOrder * 100000;
        root.rootVisible  = cv->visible;
    }
    else if (const UI::UIRect* r = world.GetComponent<UI::UIRect>(d.subject))
    {
        const UI::UICanvas scaler = d.hasScaler ? d.scaler : UI::UICanvas{};
        root.rootIsCanvas = false;
        root.scale        = UI::ComputeCanvasScale(scaler, d.designSize);
        if (d.frame.mode == UIView::RootFrame::Mode::Fill)
        {
            root.rootRect = UI::ComputeRect(*r, screen, root.scale);
        }
        else
        {
            const float w = (std::max)(0.f, d.frame.size.x * root.scale);
            const float h = (std::max)(0.f, d.frame.size.y * root.scale);
            const UI::Vec2 mn{ (d.designSize.x - w) * 0.5f, (d.designSize.y - h) * 0.5f };
            root.rootRect = UI::Rect{ mn, UI::Vec2{ mn.x + w, mn.y + h } };
        }
        root.rootVisible = true;
    }
    else
    {
        why = "the subject has neither a UICanvas nor a UIRect";
        return false;
    }
    canvasScale = root.scale;

    static thread_local std::unordered_set<Entity> t_visited;
    t_visited.clear();
    int order = 1;
    UI::LayoutSubtree(world, t_lists, root, order, t_visited, out);
    return true;
}

// ==== Game half ====
void UIViewService::PaintView(View& v, const UIView::Desc& d, World& world, float realDt)
{
    std::string why;
    const bool laidOut = LayoutDesc(d, world, v.nodes, v.canvasScale, why);
    // Hidden in the designer: laid out (the rest keeps its place), then dropped -
    // a subtree is the run after its root (DFS pre-order) deeper than it.
    if (laidOut && !d.skip.empty())
    {
        std::size_t kept = 0;
        int skipDepth = -1;
        for (std::size_t i = 0; i < v.nodes.size(); ++i)
        {
            const UI::LayoutNode& n = v.nodes[i];
            if (skipDepth >= 0 && n.cr.depth > skipDepth) continue;
            skipDepth = -1;
            if (std::find(d.skip.begin(), d.skip.end(), n.e) != d.skip.end())
            {
                skipDepth = n.cr.depth;
                continue;
            }
            if (kept != i) v.nodes[kept] = n;
            ++kept;
        }
        v.nodes.resize(kept);
    }

    v.drawList.Clear();
    const UI::Rect frame{ UI::Vec2{ 0.f, 0.f }, d.designSize };
    if (laidOut)
    {
        // The backdrop, in design pixels like everything else, so it zooms and
        // pans with the UI.
        if (d.background == UIView::Background::Checker && m_checkerSrv != 0)
        {
            UI::UITextureRef tex;
            tex.srvGpuHandle = m_checkerSrv;
            v.drawList.AddImage(tex, frame.mn, frame.mx, UI::Vec2{ 0.f, 0.f },
                                UI::Vec2{ d.designSize.x / (2.f * kCheckerCellPx),
                                          d.designSize.y / (2.f * kCheckerCellPx) },
                                UI::Color32::White(),
                                UI::UISamplerId(UI::UIWrapMode::Wrap, /*point*/ true));
        }
        else if (d.background == UIView::Background::Solid)
        {
            v.drawList.AddRectFilled(frame.mn, frame.mx, UI::Color32(d.solidRgba));
        }

        // Every node's clip intersects the frame: what leaves the screen is cut,
        // exactly as the swap chain cuts it.
        if (d.clipToFrame) v.drawList.PushClipRect(frame);
        UI::PaintOptions opt;
        opt.timeSec     = v.clock;
        opt.state       = d.state;
        opt.stateTarget = d.stateTarget;
        opt.wrapCache   = &v.wrap;
        UI::PaintLayout(world, v.nodes, v.drawList, opt);
        if (d.clipToFrame) v.drawList.PopClipRect();
    }
    const std::uint64_t designHash = v.drawList.ContentHash();

    // Images that drew nothing because their texture is still loading: the
    // picture is not final until there are none (UIImage.srvGpuHandle was
    // re-resolved from the lease earlier in this same paint window).
    std::uint32_t pendingImages = 0;
    if (laidOut)
        if (auto* imgPool = world.GetPool<UI::UIImage>())
            for (const UI::LayoutNode& n : v.nodes)
            {
                if (!n.cr.valid || n.cr.culled) continue;
                const UI::UIImage* im = imgPool->Get(n.e);
                if (im && im->visible && !im->texturePath.empty() && im->srvGpuHandle == 0) ++pendingImages;
            }

    const float tw = static_cast<float>(d.targetW);
    const float th = static_cast<float>(d.targetH);
    v.drawList.ApplyViewTransform(d.zoom, d.pan, UI::Rect{ UI::Vec2{ 0.f, 0.f }, UI::Vec2{ tw, th } },
                                  (d.debug & UIView::kDebugRunRectOff) == 0);

    // Negative arm of the uieditor_resolution pack (-uieditor_write_computed): writes the
    // designer's layout over the game screen's - exactly what the designer must never do.
    if ((d.debug & UIView::kDebugWriteComputed) != 0 && laidOut)
        if (auto* crPool = world.GetPool<UI::UIComputedRect>())
            for (const UI::LayoutNode& n : v.nodes)
                if (UI::UIComputedRect* cr = crPool->Get(n.e)) *cr = n.cr;

    // What the GPU would draw: the list in view pixels, the target and the clear.
    // Custom text shaders animate off the UI clock without moving a vertex, so with
    // one on screen a running clock is part of the picture too.
    std::uint64_t h = v.drawList.ContentHash();
    h = Mix(h, d.targetW);
    h = Mix(h, d.targetH);
    h = Mix(h, d.workspaceRgba);
    bool animatedShader = false;
    for (const UI::UIDrawCmd& c : v.drawList.Commands())
        if (c.customShaderSlot != 0) { animatedShader = true; break; }
    if (d.realtime && animatedShader)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v.clock, sizeof(bits));
        h = Mix(h, bits);
    }
    v.paintedHash = h;
    v.clearRgba   = d.workspaceRgba;
    v.hasDrawList = true;
    if (d.realtime && realDt > 0.f) v.clock += realDt;

    std::lock_guard<std::mutex> lk(m_statusMutex);
    v.needsRecord = v.fresh || v.paintedHash != v.recordedHash || (d.debug & UIView::kDebugIdleRecord) != 0;
    UIView::Status& s = v.paintStatus;
    ++s.paints;
    s.nodes         = static_cast<std::uint32_t>(v.nodes.size());
    s.verts         = static_cast<std::uint32_t>(v.drawList.Vertices().size());
    s.indices       = static_cast<std::uint32_t>(v.drawList.Indices().size());
    s.cmds          = static_cast<std::uint32_t>(v.drawList.Commands().size());
    s.effects       = static_cast<std::uint32_t>(v.drawList.Effects().size());
    s.truncated     = !v.drawList.HasRoomFor(4, 6);
    s.designHash    = designHash;
    s.canvasScale   = v.canvasScale;
    s.designSize    = d.designSize;
    s.pendingImages = pendingImages;
    s.paintedSerial = d.serial;
    s.runRectCount  = 0;
    for (const UI::GpuTextEffect& fx : v.drawList.Effects())
        if (fx.runRect[2] != 0.f && s.runRectCount < 4)
            s.runRects[s.runRectCount++] = UIView::RunRect{ fx.runRect[0], fx.runRect[1], fx.runRect[2], fx.runRect[3] };
    s.why           = why;
}

void UIViewService::PaintPending(World& world, float realDt, const UI::Vec2& mainScreen)
{
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_mainScreen = mainScreen;
    }
    struct Job { std::uint32_t id; UIView::Desc d; };
    static thread_local std::vector<Job> t_jobs;
    t_jobs.clear();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (std::uint32_t id = 0; id < m_views.size(); ++id)
        {
            const View& v = m_views[id];
            if (!v.live || v.releasePending || !v.hasRequest || !v.target.IsValid()) continue;
            if (v.requested.freeze) continue;   // a frozen view keeps its last picture (nothing painted, nothing recorded)
            t_jobs.push_back({ id, v.requested });
        }
    }
    for (const Job& j : t_jobs) PaintView(m_views[j.id], j.d, world, realDt);
}

// ==== Serial window ====
bool UIViewService::EnsureTarget(View& v, std::uint32_t w, std::uint32_t h, std::uint32_t id)
{
    if (v.target.IsValid() && v.target.desc.width == w && v.target.desc.height == h) return false;
    DestroyTarget(v);
    const std::string name = "UIView." + std::to_string(id);
    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.depth      = 1;
    td.mipLevels = 1;
    // ImGui composes in gamma space and UI.ps writes gamma-space colour, so the
    // designer shows the SDR picture in every HDR output mode.
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::RENDER_TARGET;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName = name.c_str();
    if (!m_gfx.CreateTexture(td, v.target))
    {
        v.target = {};
        return false;
    }
    v.targetBytes = m_gfx.GetTextureAllocationBytes(v.target);
    return true;
}

void UIViewService::DestroyTarget(View& v)
{
    // Frame-stamped deferred release (GraphicsDX12::DestroyTexture): the
    // resource and its descriptors outlive every frame already recorded,
    // including an ImGui draw list that captured the SRV.
    if (v.target.IsValid()) m_gfx.DestroyTexture(v.target);
    v.target      = {};
    v.targetBytes = 0;
}

bool UIViewService::EnsureChecker()
{
    if (m_checker.IsValid()) return true;
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    td.width      = 2;
    td.height     = 2;
    td.depth      = 1;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName = "UIView.Checker";
    static const std::uint8_t kPx[16] = {
        58, 58, 62, 255,   42, 42, 46, 255,
        42, 42, 46, 255,   58, 58, 62, 255,
    };
    RHI::SubresourceData sub{};
    sub.dataPtr    = kPx;
    sub.rowPitch   = 8;
    sub.slicePitch = 16;
    if (!m_gfx.CreateTexture(td, m_checker, &sub))
    {
        m_checker = {};
        return false;
    }
    m_checkerSrv = m_gfx.GetTextureSRVGpuHandle(m_checker);
    return m_checkerSrv != 0;
}

void UIViewService::ServiceFrameBoundary()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    bool anyLive = false;
    for (View& v : m_views)
    {
        if (v.releasePending)
        {
            DestroyTarget(v);
            v = View{};
            continue;
        }
        anyLive |= v.live;
    }
    m_anyLive = anyLive;
    if (!anyLive) return;

    // The second UIPass: built once, on the first live view, and KEPT - every
    // ShaderLibrary registration is a permanent shader-pool entry (4096, append
    // only), so a pass made per panel open would leak them.
    if (!m_pass)
    {
        m_pass = std::make_unique<UIPass>();
        m_pass->SetLogName("UIEditorView");   // before Init: its log lines carry the name
        m_pass->Init(m_gfx);
        m_pass->SetTargetFormat(RHI::Format::R8G8B8A8_UNORM);
        LOG_INFO("[UIVIEW] built the off-screen UI pass (first live view)");
    }
    EnsureChecker();

    // One NEW target per boundary: a new target must be drawn in its first frame
    // and the pass draws one view a frame. A second new one waits a frame with no
    // target (Srv() = 0: the panel shows nothing rather than unwritten memory) - and
    // so does any new one while an earlier fresh target is still undrawn (a frame
    // whose render half was skipped would otherwise leave two fresh ones, one of them
    // sampled unwritten).
    std::uint32_t created = 0;
    for (const View& v : m_views)
        if (v.fresh && v.target.IsValid()) ++created;
    for (std::uint32_t id = 0; id < m_views.size(); ++id)
    {
        View& v = m_views[id];
        if (!v.live || !v.hasRequest) continue;
        const std::uint32_t w = (std::min)(v.requested.targetW, kMaxSize);
        const std::uint32_t h = (std::min)(v.requested.targetH, kMaxSize);
        if (w == 0 || h == 0) continue;
        const bool needsNew = !v.target.IsValid() || v.target.desc.width != w || v.target.desc.height != h;
        if (needsNew && created >= 1) continue;
        if (EnsureTarget(v, w, h, id))
        {
            ++created;
            v.fresh = true;
            std::lock_guard<std::mutex> sl(m_statusMutex);   // m_mutex -> m_statusMutex, the one order
            v.recordedHash = 0;                              // the new target holds nothing yet
        }
    }
}

void UIViewService::ReloadShaders()
{
    if (m_pass) m_pass->ReloadShaders();
    std::lock_guard<std::mutex> lk(m_statusMutex);
    for (View& v : m_views) v.recordedHash = 0;   // redraw with the new shader
}

void UIViewService::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (View& v : m_views)
    {
        DestroyTarget(v);
        v = View{};
    }
    if (m_checker.IsValid()) m_gfx.DestroyTexture(m_checker);
    m_checker    = {};
    m_checkerSrv = 0;
    m_anyLive    = false;
}

// ==== Render half ====
void UIViewService::Execute(RHI::CommandList cl, std::uint64_t frame)
{
    if (!m_anyLive || !m_pass) return;

    // Fresh targets first (they MUST be written this frame), then, of the views whose
    // picture changed, the one drawn longest ago - the first by index would starve the
    // others behind a view that changes every frame (a realtime tab with a text shader).
    // One a frame: see the header.
    View* pick = nullptr;
    for (View& v : m_views)
        if (v.target.IsValid() && v.fresh) { pick = &v; break; }
    if (!pick)
        for (View& v : m_views)
            if (v.target.IsValid() && v.needsRecord && v.hasDrawList &&
                (!pick || v.lastRenderFrame < pick->lastRenderFrame))
                pick = &v;
    if (!pick) return;
    View& v = *pick;

    const std::uint32_t w = v.target.desc.width;
    const std::uint32_t h = v.target.desc.height;
    const float clear[4] = {
        static_cast<float>( v.clearRgba        & 0xFFu) / 255.f,
        static_cast<float>((v.clearRgba >>  8) & 0xFFu) / 255.f,
        static_cast<float>((v.clearRgba >> 16) & 0xFFu) / 255.f,
        static_cast<float>((v.clearRgba >> 24) & 0xFFu) / 255.f,
    };
    const std::uint32_t timer = m_gfx.BeginGPUTimestamp(cl, "UIEditorView");
    m_gfx.PushBarrier(RHI::GPUBarrier::Image(&v.target, RHI::ResourceState::SHADER_RESOURCE,
                                             RHI::ResourceState::RENDERTARGET), cl);
    m_gfx.ClearRenderTarget(v.target, clear, cl);
    // UIPass never clears, hence the clear above; it ends in SHADER_RESOURCE when it
    // draws, and returns false - leaving the entry state - when it has nothing to.
    const bool drew = v.hasDrawList &&
        m_pass->Execute(cl, &v.target, RHI::ResourceState::RENDERTARGET, w, h, v.clock, v.drawList);
    if (!drew)
        m_gfx.PushBarrier(RHI::GPUBarrier::Image(&v.target, RHI::ResourceState::RENDERTARGET,
                                                 RHI::ResourceState::SHADER_RESOURCE), cl);
    m_gfx.EndGPUTimestamp(cl, timer);

    v.fresh       = false;
    v.needsRecord = false;
    std::lock_guard<std::mutex> lk(m_statusMutex);
    // A fresh target with no paint yet got only the clear: it shows no
    // description, and 0 (no recorded picture) makes the next paint record.
    v.recordedHash    = v.hasDrawList ? v.paintedHash : 0;
    ++v.renders;
    v.lastRenderFrame = frame;
}
