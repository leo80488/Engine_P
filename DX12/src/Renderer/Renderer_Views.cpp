// Renderer_Views.cpp - the secondary views: the same scene through another camera, at another size,
// recorded by the same passes after the main view.
//
// A view is acquired on the main thread (AcquireView), made at the next frame boundary (ServiceViews:
// its ViewState, its FrameCB rings, its passes' per-view data), named a camera every frame on the game
// half (SetSecondaryView), and recorded by RenderAsync(viewSlot) after the main view's call
// (RenderViewsAsync). Its segment (BeginViewSegment .. EndViewSegment) is what makes the shared code
// the view's: the recording-view scope (FrameCB rings, FrameViewRead), its HDR target (the device's
// render size and HDR texture), the graph's view set, the passes' view and its plan.

#include "Renderer/Renderer.h"

#include "Graphics/GraphicsDX12.h"   // kFirstViewHdrSlot
#include "Graphics/GpuMemoryLedger.h"   // the views' VRAM
#include "Graphics/SSR/SSRSubsystem.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ViewSlot.h"
#include "Graphics/VideoMemory.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/GrassPass.h"
#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "RenderGraph/RenderPass/RTAOPass.h"
#include "RenderGraph/RenderPass/RTDIPass.h"
#include "RenderGraph/RenderPass/RTGIPass.h"
#include "RenderGraph/RenderPass/RTReflectionPass.h"
#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "PostProcess/PostProcessRuntime.h"
#include "Renderer/ViewBudget.h"
#include "Renderer/ViewCull.h"
#include "System/Check.h"
#include "System/CommandLine.h"   // -restir_reset_frame
#include "System/Log.h"

namespace
{
    // The device HDR slot of secondary view @p slot.
    uint32_t ViewHdrSlot(uint32_t slot) { return GraphicsDX12::kFirstViewHdrSlot + slot - 1; }
}

// ---- The game half's camera ----

void Renderer::SetSecondaryView(uint32_t slot, const RenderCamera& cam, uint32_t width, uint32_t height,
                                uint32_t show)
{
    if (!ENGINE_CHECK(slot != 0 && slot < RHI::kMaxViews, "Renderer::SetSecondaryView: slot %u", slot)) return;
    SecondaryFrameView& s = FrameViewWrite().secondary[slot - 1];
    s.slot     = slot;
    s.camera   = cam;
    s.vpWidth  = ViewBudget::Scaled(width);   // the ladder's half size, when the budget is short
    s.vpHeight = ViewBudget::Scaled(height);
    s.show     = show;
}

void Renderer::ClearSecondaryView(uint32_t slot)
{
    if (slot == 0 || slot >= RHI::kMaxViews) return;
    FrameViewWrite().secondary[slot - 1] = SecondaryFrameView{};
}

void Renderer::SetViewCamera(uint32_t slot, Entity camera, bool followMain, uint32_t width, uint32_t height,
                             uint32_t show)
{
    if (slot == 0 || slot >= RHI::kMaxViews || !m_secondary[slot].live) return;
    SecondaryView& v = m_secondary[slot];
    v.cameraEntity = camera;
    v.followMain   = followMain;
    v.followCamera = followMain || camera != NullEntity;
    v.reqWidth     = width;
    v.reqHeight    = height;
    v.reqShow      = show;
}

// ---- Acquire / release (main thread) and the frame boundary ----

uint32_t Renderer::AcquireView(const char* name, uint32_t width, uint32_t height)
{
    uint32_t readyViews = 0;
    for (const SecondaryView& other : m_secondary) readyViews += other.ready ? 1u : 0u;
    if (!ViewBudget::Admit(width, height, VideoMemory::Last(), readyViews))
        return 0;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        SecondaryView& v = m_secondary[slot];
        // A released view keeps its slot until the boundary has given its resources back.
        if (v.live || v.pendingRelease || v.ready) continue;
        v.live = true;
        v.name = name && *name ? name : "view";
        ++v.lease;
        v.renders.store(0, std::memory_order_relaxed);
        LOG_INFO("[VIEW] slot=%u '%s' acquired (lease %u)", slot, v.name.c_str(), v.lease);
        return slot;
    }
    LOG_WARNING("[VIEW] no free secondary view: all %u slots are taken", RHI::kMaxViews - 1);
    return 0;
}

void Renderer::ReleaseView(uint32_t slot)
{
    if (slot == 0 || slot >= RHI::kMaxViews || !m_secondary[slot].live) return;
    SecondaryView& v = m_secondary[slot];
    v.live           = false;
    v.pendingRelease = true;
    v.followCamera   = false;
    v.cameraEntity   = NullEntity;
    ClearSecondaryView(slot);
    PostProcess::Runtime::Get().ClearViewResolved(slot);
    if (m_pickingPass) m_pickingPass->CancelView(slot);
    LOG_INFO("[VIEW] slot=%u '%s' released", slot, v.name.c_str());
}

void Renderer::ServiceViews()
{
    // The budget ladder: over the video memory budget, the views render at half size, then are suspended (their
    // resources, and the heaps they shared, given back until the budget has room again).
    uint32_t readyViews = 0;
    for (const SecondaryView& other : m_secondary) readyViews += other.ready ? 1u : 0u;
    const bool suspended = ViewBudget::Step(VideoMemory::Last(), readyViews) == ViewBudget::Stage::Suspended;
    for (size_t i = 0; i < m_retiredViewSSR.size();)
    {
        if (--m_retiredViewSSR[i].second == 0)
            m_retiredViewSSR.erase(m_retiredViewSSR.begin() + static_cast<ptrdiff_t>(i));
        else
            ++i;
    }
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        SecondaryView& v = m_secondary[slot];
        if (v.pendingRelease)
        {
            v.pendingRelease = false;
            if (v.ready) FreeViewResources(slot);
        }
        else if (v.live && v.ready && suspended)
            FreeViewResources(slot);   // it stays live: the ladder brings it back
        if (v.live && !v.ready && !suspended)
        {
            // What it makes here is its own VRAM (Render.views()' vramBytes), as what its segment makes.
            const uint32_t attributed = GpuLedger::SetViewAttribution(slot);
            v.state   = std::make_unique<ViewState>(v.name.c_str(), slot);
            // A copy of the main view's fit: its frame index (the PCF rotation's phase) then advances in
            // step with the main view's, once a frame.
            v.shadows = m_shadowSystem ? std::make_unique<ShadowSystem>(*m_shadowSystem)
                                       : std::make_unique<ShadowSystem>();
            // Its own SSR chain; its temporal pass's history lives in the view, as the main chain's does
            // in the main view.
            if (m_ssrSubsystem)
            {
                v.ssr = std::make_unique<SSRSubsystem>();
                v.ssr->Init(m_gfx);
                v.ssr->TemporalPass()->BindView(*v.state);
            }
            v.pyramidBuiltLastFrame = false;
            RHI::EnsureViewSlot(m_gfx, slot);
            if (m_toneMapPass) m_toneMapPass->ReserveLutSlots(slot + 1);   // its grading may differ from the main view's
            // The passes make their per-view data here, on the main thread, not in the view's segment on
            // the render half: Lua and the editor read it (GetViewOutputSrv) from this thread.
            m_passes.BindView(*v.state);
            m_passes.BindView(m_mainView);
            v.ready = true;
            GpuLedger::SetViewAttribution(attributed);
            LOG_INFO("[VIEW] slot=%u '%s' ready: %zu histories, %zu view data", slot, v.name.c_str(),
                     v.state->HistoryCount(), v.state->DataCount());
        }
    }
}

void Renderer::FreeViewResources(uint32_t slot)
{
    SecondaryView& v = m_secondary[slot];
    // Everything goes through the device's deferred release: frames in flight may still read it.
    if (v.state) v.state->ReleaseHistories(m_gfx);
    m_graph.ReleaseViewSet(m_gfx, slot);
    RHI::ReleaseViewSlot(m_gfx, slot);
    if (v.width) m_gfx.DestroyHdrTarget(ViewHdrSlot(slot));
    v.state.reset();
    v.shadows.reset();
    for (RG::PassTexture& t : v.ppPool.tex) t.Release(m_gfx);
    v.ppPool.w = v.ppPool.h = 0;
    // Its textures go through the deferred release; its alias heap goes when the frames in flight that
    // may read it have drained.
    if (v.ssr)
    {
        v.ssr->ReleaseTextures();
        m_retiredViewSSR.emplace_back(std::move(v.ssr), RHI::kFramesInFlight + 1u);
    }
    v.ready  = false;
    v.width  = v.height = 0;
    v.breaks = 0;
    v.drawList.clear();
    v.drawList.shrink_to_fit();
    // The last view gone: the heaps the views shared go with it (their textures were released above).
    bool anyReady = false;
    for (const SecondaryView& other : m_secondary) anyReady = anyReady || other.ready;
    if (!anyReady) m_graph.ReleaseViewHeaps();
}

Renderer::ViewInfo Renderer::GetViewInfo(uint32_t slot) const
{
    ViewInfo info;
    if (slot == 0 || slot >= RHI::kMaxViews) return info;
    const SecondaryView& v = m_secondary[slot];
    info.live    = v.live;
    info.lease   = v.lease;
    info.renders = v.renders.load(std::memory_order_relaxed);
    info.width   = v.width;
    info.height  = v.height;
    info.name    = v.name.c_str();
    // Its VRAM: what its setup and segments made (the ledger's attribution; 0 without the profiler), and its
    // graph textures - its own committed ones and the ones in the views' alias heaps.
    info.vramBytes = GpuLedger::ViewBytes(slot);
    if (v.ready)
    {
        const RG::RenderGraph::ViewSetBytes graph = m_graph.GetViewSetBytes(m_gfx, slot);
        info.graphBytes       = graph.committed;
        info.graphPlacedBytes = graph.placed;
    }
    return info;
}

bool Renderer::GetViewFrameCamera(uint32_t slot, ViewFrameCamera& out) const
{
    if (slot == 0 || slot >= RHI::kMaxViews) return false;
    const SecondaryFrameView& s = FrameViewWrite().secondary[slot - 1];
    if (s.slot != slot || s.vpWidth == 0 || s.vpHeight == 0) return false;
    // Its frame as its segment builds it (BeginViewSegment): the main one with its camera and sizes.
    FrameView fv = FrameViewWrite();
    fv.camera   = s.camera;
    fv.vpWidth  = fv.renderWidth  = s.vpWidth;
    fv.vpHeight = fv.renderHeight = s.vpHeight;
    DirectX::XMMATRIX view, proj;
    BuildCameraViewProjFor(fv, view, proj);
    DirectX::XMStoreFloat4x4(&out.view, view);
    DirectX::XMStoreFloat4x4(&out.viewProjNoJitter, DirectX::XMMatrixMultiply(view, proj));
    out.width  = s.vpWidth;
    out.height = s.vpHeight;
    out.show   = s.show;
    return true;
}

const RHI::Texture* Renderer::GetViewOutputTexture(uint32_t slot) const
{
    if (slot == 0 || slot >= RHI::kMaxViews || !m_toneMapPass) return nullptr;
    const SecondaryView& v = m_secondary[slot];
    if (!v.ready || !v.state || v.renders.load(std::memory_order_relaxed) == 0) return nullptr;
    return m_toneMapPass->FinalOutputOf(*v.state);
}

uint64_t Renderer::GetViewOutputSrv(uint32_t slot) const
{
    const RHI::Texture* t = GetViewOutputTexture(slot);
    return t ? m_gfx.GetTextureSRVGpuHandle(*t) : 0;
}

ViewState& Renderer::RecordingViewState()
{
    const uint32_t slot = RHI::CurrentRecordingView().slot;
    if (slot != 0 && slot < RHI::kMaxViews && m_secondary[slot].state) return *m_secondary[slot].state;
    return m_mainView;
}

SSRSubsystem* Renderer::RecordingSSR()
{
    const uint32_t slot = RHI::CurrentRecordingView().slot;
    if (slot != 0 && slot < RHI::kMaxViews) return m_secondary[slot].ssr.get();
    return m_ssrSubsystem.get();
}

// ---- The render half ----

FramePlan::Decisions Renderer::SecondaryViewPlan(const FramePlan::Decisions& main, uint32_t width,
                                                 uint32_t height) const
{
    FramePlan::Decisions d = main;
    // The main view's path where the view's own working sets are up (its segment made them): the path tracer
    // with this pass's own path tracer (its RTXDI mode is the main view's), the ReSTIR shells off the path
    // tracer, ReSTIR GI not while SHaRC runs (a world cache the main view updates); the raster path else.
    d.pt        = main.pt && m_pathTracePass && !m_pathTracePass->UsesRtxdi() && !m_pathTracePass->GetInertReason();
    d.ptOwns    = d.pt && main.ptOwns;
    d.ptReaders = d.ptOwns ? main.ptReaders : 0u;
    d.path      = d.pt ? main.path : FramePlan::Path::Raster;
    d.di = main.di && !main.ptOwns && m_rtDiPass && !m_rtDiPass->GetInertReason();
    d.gi = main.gi && !main.ptOwns && m_rtGiPass && !m_rtGiPass->GetSharcStatus().effective
           && !m_rtGiPass->GetInertReason();
    // The upscalers and their guides are one context / one Streamline viewport: the main view's.
    d.rr = d.rrRuns = d.fsr = d.dlssWanted = d.dlss = false;
    d.fsrResolved = d.dlssResolved = false;
    d.taaStandIn  = false;
    d.rrGuidesWanted = d.rrGuides = false;
    // The RT reflections and their screen tier: where the main view runs them and the view's own working
    // set is up (its segment made it at its size); the screen tier also needs the view's pyramid of last
    // frame, the glass's front layer the traced tier and its own instance's working set (RenderAsync adds
    // the view's draws).
    const uint32_t slot = RHI::CurrentRecordingView().slot;
    const SSRSubsystem* ssr = (slot != 0 && slot < RHI::kMaxViews) ? m_secondary[slot].ssr.get() : nullptr;
    const bool reflUnified = m_rtReflectionPass && m_rtReflectionPass->IsUnifiedAvailable();
    d.rtr     = main.rtr && reflUnified;
    d.ssrTier = main.ssrTier && reflUnified && ssr && ssr->GetSceneColorPyramidSrv() != 0;
    d.front   = main.front && d.rtr && m_rtReflectionFrontPass && m_rtReflectionFrontPass->IsUnifiedAvailable();
    // Its own SSR chain (the raster path: always wanted, as off the path tracer for the main view).
    d.ssrChain = true;
    // The traced sun shadow and AO: where the main view runs them and the view's own working set is up (its
    // segment made it at its size, BeginViewSegment). While RTAO's is not, XeGTAO stands in - RTAO's own
    // fallback, whose working set is kept while SSAO is on.
    d.rtShadow = main.rtShadow && m_rtShadowPass && m_rtShadowPass->IsReady()
                 && m_rtShadowPass->GetUpcomingOutputSrv() != 0;
    d.rtao     = main.rtao && m_rtaoPass && m_rtaoPass->IsReady() && m_rtaoPass->GetUpcomingOutputSrv() != 0;
    d.xegtao   = main.xegtao || (main.rtao && !d.rtao);
    // csm stays the main view's answer: the view draws its own cascades (its fit) in its segment.
    d.postW = width;
    d.postH = height;
    return d;
}

bool Renderer::BeginViewSegment(uint32_t slot)
{
    SecondaryView& v = m_secondary[slot];
    const FrameView&          main = FrameViewRead();   // no scope yet: the main view's frame
    const SecondaryFrameView& s    = main.secondary[slot - 1];
    if (!v.ready || !v.state || s.slot != slot || s.vpWidth == 0 || s.vpHeight == 0) return false;

    // Its frame: the main one with its camera and sizes (no upscaler: render = display).
    FrameView& fv = m_viewFrames[slot];
    fv = main;
    fv.camera         = s.camera;
    fv.vpWidth        = fv.renderWidth  = s.vpWidth;
    fv.vpHeight       = fv.renderHeight = s.vpHeight;
    fv.viewBreaks     = fv.viewBreaksLoud = 0;
    fv.dlssResetSent  = false;
    v.show            = s.show;

    // Its HDR target at its size: the device's render size and HDR texture while it is pushed.
    const uint32_t hdr = ViewHdrSlot(slot);
    if (v.width != s.vpWidth || v.height != s.vpHeight)
    {
        m_gfx.CreateHdrTarget(hdr, s.vpWidth, s.vpHeight);
        if (!ENGINE_CHECK_ONCE(m_gfx.GetHdrTexture(hdr).IsValid(),
                               "[VIEW] slot=%u: no %ux%u HDR target", slot, s.vpWidth, s.vpHeight))
            return false;
        if (v.width) v.breaks |= HistoryReset::Resize;
        v.width  = s.vpWidth;
        v.height = s.vpHeight;
        LOG_INFO("[VIEW] slot=%u '%s' renders at %ux%u (HDR slot %u)", slot, v.name.c_str(), v.width, v.height, hdr);
    }
    if (!ENGINE_CHECK_ONCE(m_gfx.PushHdrTarget(hdr), "[VIEW] slot=%u: its HDR target was not pushed", slot))
        return false;
    m_viewHdrPushed = true;
    // Its SSR chain at its size (the main chain is resized in BeginFrame).
    if (v.ssr) v.ssr->OnResize(s.vpWidth, s.vpHeight);

    m_viewScope.emplace(slot);
    m_viewAttributed = GpuLedger::SetViewAttribution(slot);   // what the segment makes is the view's VRAM
    m_graph.BindViewSet(m_gfx, slot);
    m_passes.BindView(*v.state);
    // A pick or an object-ID image asked of it: its targets at its size, before it records.
    if (m_pickingPass) m_pickingPass->PrepareBoundView(m_gfx, s.vpWidth, s.vpHeight);
    // Its traced sun-shadow and AO working sets at its size, before its plan asks whether they are up.
    if (m_rtShadowPass) m_rtShadowPass->EnsureViewResources(s.vpWidth, s.vpHeight);
    if (m_rtaoPass)     m_rtaoPass->EnsureViewResources(s.vpWidth, s.vpHeight);
    // Its RT-reflection working sets while the main view traces reflections or runs their screen tier, and
    // the front layer's while the main view traces its glass (m_plan is still the main view's here).
    if (m_rtReflectionPass)
        m_rtReflectionPass->EnsureViewResources(s.vpWidth, s.vpHeight, m_plan.rtr || m_plan.ssrTier);
    if (m_rtReflectionFrontPass)
        m_rtReflectionFrontPass->EnsureViewResources(s.vpWidth, s.vpHeight, m_plan.front);
    // Its path tracer's while the main view path-traces with its own, and its ReSTIR working sets while the
    // main view runs them off the path tracer (GI without SHaRC).
    if (m_pathTracePass)
        m_pathTracePass->EnsureViewResources(s.vpWidth, s.vpHeight, m_plan.pt && !m_pathTracePass->UsesRtxdi());
    if (m_rtDiPass) m_rtDiPass->EnsureViewResources(s.vpWidth, s.vpHeight, m_plan.di && !m_plan.ptOwns);
    if (m_rtGiPass)
        m_rtGiPass->EnsureViewResources(s.vpWidth, s.vpHeight,
                                        m_plan.gi && !m_plan.ptOwns && !m_rtGiPass->GetSharcStatus().effective);
    m_mainPlanDuringView = m_plan;
    m_plan = SecondaryViewPlan(m_mainPlanDuringView, s.vpWidth, s.vpHeight);

    // Its camera-derived frame data, into its rings (the scope's FrameCB copies of the main view's):
    // its own CSM fit too, which ShadowPass draws into the shared cascade array in this segment.
    ViewFrameTarget t{ fv, *v.state, m_plan };
    t.perView          = m_perObjectCB.Current(m_gfx);
    t.light            = m_lightCB.Current(m_gfx);
    t.cascades         = m_plan.csm ? v.shadows.get() : nullptr;
    if (m_shadowPass && t.cascades) m_shadowPass->SetShadowSystem(t.cascades);
    t.slot             = m_gfx.GetFrameIndex();
    t.instanceRemapIdx = RetainedInstancesActive() ? m_instanceRemapHeapIdx[t.slot] : 0u;
    BuildViewFrame(t);
    // Its light clusters: its camera's grid, and LightCB's cluster params.
    if (m_clusterPass) SetClusterCamera(fv, v.state.get(), t.light);
    if (m_decalPass)   SetDecalCamera(fv, v.state.get());
    // Its terrain and grass cull from its frustum (the main view's planes are the game half's).
    if (m_terrainPass && m_terrainCB.IsValid())
        if (auto* tcb = m_terrainCB.Current(m_gfx))
            for (int p = 0; p < 6; ++p)
                tcb->frustumPlanes[p] = { fv.view.frustum[p].normal.x, fv.view.frustum[p].normal.y,
                                          fv.view.frustum[p].normal.z, fv.view.frustum[p].distance };
    // Its hair fringe mark: the offsets follow its camera.
    if (m_hairMarkPass)
        m_hairMarkPass->SetViewCamera(*v.state, fv.view.viewProjMatrix, fv.view.viewMatrix, fv.view.cameraPosition);
    if (m_grassPass)
    {
        const float pos[3] = { fv.view.cameraPosition.x, fv.view.cameraPosition.y, fv.view.cameraPosition.z };
        float planes[6][4];
        for (int p = 0; p < 6; ++p)
        {
            planes[p][0] = fv.view.frustum[p].normal.x;
            planes[p][1] = fv.view.frustum[p].normal.y;
            planes[p][2] = fv.view.frustum[p].normal.z;
            planes[p][3] = fv.view.frustum[p].distance;
        }
        m_grassPass->SetViewCamera(*v.state, pos, planes);
    }

    // What it draws: the scene's packets its frustum holds - the main camera's lists and the
    // duplicates the classify kept for it - with their natural filters, every viewMask its own. The
    // blended lists last: the view re-sorts everything from the first transparent packet on, and the
    // graph takes each filter's first run (RenderContext::SetDrawList).
    RG::SceneView view;
    view.name = v.name.c_str();
    CullPlanesFrom(fv.view.frustum, view.cull.volume);
    view.cull.eye[0]     = fv.view.cameraPosition.x;
    view.cull.eye[1]     = fv.view.cameraPosition.y;
    view.cull.eye[2]     = fv.view.cameraPosition.z;
    view.cull.forward[0] = fv.view.cameraForward.x;
    view.cull.forward[1] = fv.view.cameraForward.y;
    view.cull.forward[2] = fv.view.cameraForward.z;
    view.cull.viewBit    = ViewBit::MainCamera;
    ViewCullRequest req;
    req.lists[0] = DrawFilter::Opaque;
    req.lists[1] = DrawFilter::SecondaryView;       req.secondary[1] = true;
    req.lists[2] = DrawFilter::Custom;
    req.lists[3] = DrawFilter::SecondaryViewCustom; req.secondary[3] = true;
    req.lists[4] = DrawFilter::Water;   // never dropped off the main screen: no duplicate
    req.lists[5] = DrawFilter::Transparent;
    req.lists[6] = DrawFilter::SecondaryViewBlend;  req.secondary[6] = true;
    req.listCount       = 7;
    req.naturalFilters  = true;
    req.rewriteViewMask = true;
    req.sortTransparent = true;
    v.drawList.clear();   // CullView appends
    CullView(view, req, v.drawList, nullptr);
    // The editor's picking outline is an editor overlay: only a view that shows them keeps it.
    if (!(v.show & static_cast<uint32_t>(RG::ShowFlag::EditorOverlays)))
        std::erase_if(v.drawList, [](const DrawPacket& dp)
                      { return dp.filter == DrawFilter::Custom && dp.isPickingOutline; });
    // Its outlines at its camera and size.
    if (m_outlinePass)
        m_outlinePass->SetViewCamera(*v.state, fv.camera.nearZ, fv.camera.farZ, fv.vpWidth, fv.vpHeight);
    if (m_outlineGeometryPass)
        m_outlineGeometryPass->SetViewCamera(*v.state, fv.camera.nearZ, fv.camera.farZ, fv.vpWidth, fv.vpHeight);
    // Its clouds, from its LightCB's camera (the main view's are pushed the same way, Renderer_Frame).
    if (m_cloudPass && t.light)
    {
        DirectX::XMFLOAT4X4 invVP;
        std::memcpy(&invVP, t.light->invViewProj, sizeof(invVP));
        const DirectX::XMFLOAT3 camPos{ t.light->cameraPos[0], t.light->cameraPos[1], t.light->cameraPos[2] };
        m_cloudPass->SetViewCamera(*v.state, invVP, camPos, fv.camera.nearZ, fv.camera.farZ);
    }
    // Its aerial perspective, the same camera (the main view's: Renderer_Frame).
    if (m_skyIBLPass && t.light)
    {
        const DirectX::XMFLOAT3 camPos{ t.light->cameraPos[0], t.light->cameraPos[1], t.light->cameraPos[2] };
        const DirectX::XMFLOAT3 camFwd{ t.light->cameraForward[0], t.light->cameraForward[1],
                                        t.light->cameraForward[2] };
        m_skyIBLPass->SetViewCameraForAerial(*v.state, camPos, camFwd, t.light->invViewProj);
    }
    return true;
}

void Renderer::Render_BindViewResources(uint32_t slot, uint32_t frameSlot)
{
    SecondaryView& v = m_secondary[slot];
    uint32_t breaks = v.breaks;
    uint32_t loud   = v.breaks;
    v.breaks = 0;
    if (!FrameViewRead().camera.historyValid)
        breaks |= HistoryReset::CameraCut;
    // -restir_reset_frame=N (diagnostic): every view's temporal history restarts at render frame N, as the
    // main view's does (Render_BindFrameResources).
    static const int s_resetFrame = Sys::CommandLine::Get().GetInt("-restir_reset_frame", -1);
    if (s_resetFrame >= 0 && FrameViewRead().currentFrame == static_cast<uint64_t>(s_resetFrame))
    {
        breaks |= HistoryReset::HardCut;
        loud   |= HistoryReset::HardCut;
    }
    v.state->BeginFrame(FrameViewRead().currentFrame, breaks, loud);
    BindViewInputs(frameSlot, v.drawList, v.name.c_str(), *v.state, /*primary=*/false, v.show);
    if (m_lightingPass && m_skyIBLPass)
        m_lightingPass->SetViewAerialPerspective(m_skyIBLPass->GetViewAerialSrvHandle());
    // Its decals are culled against its own cluster grid.
    if (m_decalPass && m_clusterPass)
        m_decalPass->SetBoundClusterAABBSRV(m_clusterPass->GetClusterAABBSRVHandle());
    Render_BindVfxFrameData();
}

void Renderer::EndViewSegment(uint32_t slot)
{
    if (m_shadowPass) m_shadowPass->SetShadowSystem(m_shadowSystem.get());
    m_plan = m_mainPlanDuringView;
    m_passes.BindView(m_mainView);
    m_graph.BindViewSet(m_gfx, 0);
    GpuLedger::SetViewAttribution(m_viewAttributed);
    m_viewScope.reset();
    if (m_viewHdrPushed)
    {
        m_gfx.PopHdrTarget();
        m_viewHdrPushed = false;
    }
    m_secondary[slot].renders.fetch_add(1, std::memory_order_relaxed);
}

Sys::OwnerTask Renderer::RenderViewsAsync(Sys::OwnerTaskQueue& owner,
    std::optional<Sys::ExecutionDomain::Token> token, RHI::CommandList after, RHI::CommandList& last)
{
    last        = {};
    m_viewAfter = after;
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        const SecondaryFrameView& s = FrameViewRead().secondary[slot - 1];
        if (s.slot != slot || !m_secondary[slot].ready) continue;
        RHI::CommandList viewLast{};
        co_await RenderAsync(owner, token, viewLast, slot).Start(owner, token, "Renderer.RenderView");
        // Its placed render targets and depth parked in their write states after its last list: the next
        // view's graph (or its own next frame's) activates bytes they share with a Discard, no transition.
        if (viewLast.IsValid())
            if (const RHI::CommandList park = m_graph.ParkViewSet(m_gfx, slot, viewLast); park.IsValid())
                viewLast = park;
        if (viewLast.IsValid())
        {
            m_viewAfter = viewLast;
            last        = viewLast;
        }
    }
    m_viewAfter      = {};
    m_framePromoteCL = {};
}
