#include "Graphics/SSR/SSRSubsystem.h"
#include "RenderGraph/RenderGraph.h"   // RenderGraph::PlanesEnabled
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"   // CreateTexturePlaced + CreateHeap (alias path)
#include "Graphics/DepthTrace.h"     // DX12_DEPTHTRACE command-list roster
#include "Graphics/ViewSlot.h"       // RHI::CurrentRecordingView - whose HDR is in scope
#include "System/Check.h"            // ENGINE_CHECK_ONCE - HDR push-scope guards
#include "System/Log.h"
#include "Graphics/SSR/SSRTracePass.h"
#include "Graphics/SSR/SSRResolvePass.h"
#include "Graphics/SSR/SSRTemporalPass.h"
#include "Graphics/SSR/SSRUpsamplePass.h"
#include "Graphics/SSR/SSRCompositePass.h"
#include "Graphics/SSR/SSRDepthHierarchyPass.h"
#include "Graphics/SSR/SceneColorPyramidPass.h"

#include <cstring>

namespace
{
// The HDR scene colour this chain reads and parks is the recording view's: the main view's outside every
// push scope, a secondary view's inside the one push its segment made (its own target). Any other push
// (a preview graph's) would point the HDR accesses at the wrong target.
bool HdrIsRecordingViews(IGraphicsDevice& gfx)
{
    const int depth = static_cast<GraphicsDX12&>(gfx).HdrScopeDepth();
    return RHI::CurrentRecordingView().slot == 0 ? depth == 0 : depth == 1;
}
}

// ==== Lifecycle ====
SSRSubsystem::SSRSubsystem()  = default;
SSRSubsystem::~SSRSubsystem() = default;

RG::PassLifecycle* SSRSubsystem::TemporalPass() const { return m_temporal.get(); }

void SSRSubsystem::Init(IGraphicsDevice& gfx)
{
    // Construction order = pipeline order. The sub-passes don't depend on each other at
    // Init time, but this keeps the destructor and log sequence readable.
    m_trace = std::make_unique<SSRPass>();
    m_trace->Init(gfx);

    m_resolve = std::make_unique<SSRResolvePass>();
    m_resolve->Init(gfx);

    m_temporal = std::make_unique<SSRTemporalPass>();
    m_temporal->Init(gfx);

    m_upsample = std::make_unique<SSRUpsamplePass>();
    m_upsample->Init(gfx);

    m_composite = std::make_unique<SSRCompositePass>();
    m_composite->Init(gfx);

    m_depthHier = std::make_unique<SSRDepthHierarchyPass>();
    m_depthHier->Init(gfx);

    // Karis-firefly HDR mip chain for trace cone-footprint sampling.
    m_sceneColorPyr = std::make_unique<SceneColorPyramidPass>();
    m_sceneColorPyr->Init(gfx);
}

void SSRSubsystem::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_trace)         m_trace->ReloadShaders(gfx);
    if (m_resolve)       m_resolve->ReloadShaders(gfx);
    if (m_temporal)      m_temporal->ReloadShaders(gfx);
    if (m_upsample)      m_upsample->ReloadShaders(gfx);
    if (m_composite)     m_composite->ReloadShaders(gfx);
    if (m_depthHier)     m_depthHier->ReloadShaders(gfx);
    if (m_sceneColorPyr) m_sceneColorPyr->ReloadShaders(gfx);
}

void SSRSubsystem::ReleaseTextures()
{
    if (m_trace)         m_trace->ReleaseTextures();
    if (m_resolve)       m_resolve->ReleaseTextures();
    if (m_upsample)      m_upsample->ReleaseTextures();
    if (m_depthHier)     m_depthHier->ReleaseTextures();
    if (m_sceneColorPyr) m_sceneColorPyr->ReleaseTextures();
}

uint64_t SSRSubsystem::OnResize(uint32_t renderW, uint32_t renderH)
{
    if (!m_trace) return 0;

    m_trace->EnsureTexture(renderW, renderH);
    // With scratch aliasing the resolve snapshot + color must be the PLACED variant Render()
    // creates with the alias heap: skip the committed creation here so it is not created
    // first and then early-out'd.
    if (m_resolve && !kSSREnableScratchAliasing) m_resolve->EnsureTextures(renderW, renderH);
    if (m_temporal) m_temporal->EnsureTextures(renderW, renderH);
    if (m_upsample) m_upsample->EnsureTexture(renderW, renderH);

    // Preferred binding is the upsample output (Water.ps dampens its sky term by
    // 1 - ssrConf, one frame latent); before the upsample exists, the raw trace.
    // EnsureTexture above may have RECREATED these targets, so the handle can differ from
    // the last call's: Renderer must re-derive LightCB.ssrResultIdx from it this frame
    // (see kLightCB_SSRResultIdx in RootSignatureLayout.h).
    if (m_upsample && m_upsample->GetColorSrv())
        return m_upsample->GetColorSrv();
    return m_trace->GetResultSrv();
}

uint64_t SSRSubsystem::GetFinalColorSrv() const
{
    if (m_upsample) return m_upsample->GetColorSrv();
    if (m_trace)    return m_trace->GetResultSrv();
    return 0;
}

uint64_t SSRSubsystem::GetSceneColorPyramidSrv() const
{
    // 0 until the first build (SceneColorPyramidPass gates on m_built).
    return m_sceneColorPyr ? m_sceneColorPyr->GetSrvHandle() : 0;
}

uint32_t SSRSubsystem::GetSceneColorPyramidMips() const
{
    return m_sceneColorPyr ? m_sceneColorPyr->GetMipCount() : 0;
}

// The same textures as the two SRVs above.
const RHI::Texture* SSRSubsystem::SceneColorPyramidTexture() const
{
    return m_sceneColorPyr ? m_sceneColorPyr->GetTexture() : nullptr;
}

const RHI::Texture* SSRSubsystem::FinalColorTexture() const
{
    if (m_upsample) return m_upsample->GetColorTexture();
    if (m_trace)    return m_trace->GetResultTexture();
    return nullptr;
}

// ==== Transient scratch-alias heap (kSSREnableScratchAliasing) ====
bool SSRSubsystem::EnsureAliasHeap(GraphicsDX12& gfx, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (m_aliasHeap && w == m_aliasHeapW && h == m_aliasHeapH) return true;

    // Size for one full-res RGBA16F SR|UAV 2D texture: both the snapshot (SR-only) and the
    // resolve color (SR|UAV) fit; they alias at offset 0.
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_RESOURCE_ALLOCATION_INFO ai =
        gfx.GetDevice()->GetResourceAllocationInfo(0, 1, &rd);
    if (ai.SizeInBytes == 0 || ai.SizeInBytes == UINT64_MAX) return false;

    // Retire the old heap: its placed resources are about to be DestroyTexture'd
    // (deferred kFrameCount frames). Free the heap one cycle later so it always
    // outlives the placed resources referencing it (the key TDR-avoidance rule).
    if (m_aliasHeap)
        m_retiredHeaps.emplace_back(std::move(m_aliasHeap), GraphicsDX12::kFrameCount + 1u);

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes        = ai.SizeInBytes;
    hd.Properties.Type    = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64KB
    hd.Flags              = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES
                          | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    if (FAILED(gfx.GetDevice()->CreateHeap(&hd, IID_PPV_ARGS(&m_aliasHeap))))
    {
        LOG_ERROR("SSRSubsystem: alias-heap CreateHeap failed (%llu bytes)",
                  (unsigned long long)ai.SizeInBytes);
        m_aliasHeap.Reset();
        return false;
    }
    m_aliasHeapSize = ai.SizeInBytes;
    GpuLedger::TrackHeap(m_aliasHeap.Get(), Diag::Prof::GpuMemCat::TransientHeap, "SSR.AliasHeap");
    m_aliasHeapW = w; m_aliasHeapH = h;
    return true;
}

void SSRSubsystem::TickAliasHeap()
{
    for (auto it = m_retiredHeaps.begin(); it != m_retiredHeaps.end(); )
    {
        if (--it->second == 0) it = m_retiredHeaps.erase(it);
        else                   ++it;
    }
}

// ==== Render: Renderer phase 4.6 (trace chain) + 4.7 (composite) ====
// The chain is mode-gated (LegacyMode). The snapshot + scene-color pyramid run every
// reflection frame: they feed the next frame's unified trace in RTReflectionPass. The
// march / denoise passes and the composite run only for water frames (FullAdditive) or
// as the whole-screen fallback when the unified pass is unavailable (FullFold).
RHI::CommandList SSRSubsystem::Render(IGraphicsDevice& gfx,
                                      const FrameContext& ctx,
                                      RHI::CommandList colorLastCL,
                                      RHI::CommandList& depthLastCL,
                                      LegacyMode mode)
{
    // Post-graph GBuffer_Depth chain (see SSRSubsystem.h and Phase 4.5 in
    // Renderer::RenderAsync). Advances ONLY for lists that really transition depth, so the
    // pyramid-only path adds no serialisation.
    auto chainDepth = [&](RHI::CommandList cl)
    {
        if (!cl.IsValid()) return;
        if (depthLastCL.IsValid() && !(depthLastCL == cl))
            gfx.AddCommandListDependency(cl, depthLastCL);
        depthLastCL = cl;
    };

    const bool legacy = (mode != LegacyMode::PyramidOnly);
    if (!m_resolve) return {};                     // snapshot owner, always needed
    if (legacy && (!m_trace || !m_depthHier)) return {};
    if (!ctx.graph) return {};

    auto& graph = *ctx.graph;

    const RHI::Texture* depthTex    = graph.GetPhysicalTexture(ctx.depthHandle);
    const RHI::Texture* normalTex   = graph.GetPhysicalTexture(ctx.normalHandle);
    const RHI::Texture* surfaceTex  = graph.GetPhysicalTexture(ctx.surfaceHandle);
    const RHI::Texture* velocityTex = graph.GetPhysicalTexture(ctx.velocityHandle);
    const RHI::Texture* reflectionSourceTex =
        graph.GetPhysicalTexture(ctx.reflectionSourceHandle);
    // Grass-free pre-GrassPass snapshot: the Hi-Z march source (see FrameContext).
    const RHI::Texture* traceDepthTex = graph.GetPhysicalTexture(ctx.traceDepthHandle);
    if (!depthTex || !normalTex || !surfaceTex) return {};

    const uint32_t rw = gfx.GetRenderWidth();
    const uint32_t rh = gfx.GetRenderHeight();

    if (m_trace) m_trace->EnsureTexture(rw, rh);
    // Legacy-only Hi-Z (the unified pass owns its own instance): skip the allocation
    // entirely on pyramid-only frames.
    if (legacy) m_depthHier->EnsureTexture(rw, rh);
    if (kSSREnableScratchAliasing)
    {
        // snapshot + color share one heap (lifetime-disjoint, same size).
        auto& dx12 = static_cast<GraphicsDX12&>(gfx);
        TickAliasHeap();
        if (EnsureAliasHeap(dx12, rw, rh))
            m_resolve->EnsureTextures(rw, rh, m_aliasHeap.Get(), /*snapshot*/0, /*color*/0);
        else
            m_resolve->EnsureTextures(rw, rh); // heap alloc failed -> committed fallback
    }
    else
    {
        m_resolve->EnsureTextures(rw, rh);
    }

    // ---- Per-frame camera state (jittered) ----
    using namespace DirectX;
    SSRPass::Camera traceCam{};
    traceCam.viewProj = ctx.viewProj;
    {
        XMMATRIX vp = XMLoadFloat4x4(&traceCam.viewProj);
        XMStoreFloat4x4(&traceCam.invViewProj, XMMatrixInverse(nullptr, vp));
    }
    traceCam.cameraPos = ctx.cameraPos;
    traceCam.nearZ     = ctx.nearZ;
    traceCam.farZ      = ctx.farZ;
    // One frame index shared by trace / resolve / temporal so their sub-pixel
    // jitter tables index the same slot. Post-increment AFTER the three Set*
    // calls so this-frame's value is used everywhere.
    const uint32_t frameIdx = m_frameIndex;
    if (m_trace)
    {
        m_trace->SetCamera(traceCam);
        m_trace->SetFrameIndex(frameIdx);
        // Hi-Z mip count = 1 + floor(log2(max(w,h))).
        uint32_t mip = 1, dim = (rw > rh) ? rw : rh;
        while (dim > 1) { dim >>= 1; mip++; }
        m_trace->SetHiZMipCount(mip);
    }

    SSRResolvePass::Camera rcam{};
    rcam.invViewProj = traceCam.invViewProj;
    rcam.cameraPos   = traceCam.cameraPos;
    rcam.nearZ       = traceCam.nearZ;
    rcam.farZ        = traceCam.farZ;
    m_resolve->SetCamera(rcam);
    m_resolve->SetFrameIndex(frameIdx);

    SSRTemporalPass::Camera tcam{};
    tcam.invViewProj  = traceCam.invViewProj;
    tcam.prevViewProj = ctx.prevViewProjJittered;
    tcam.nearZ        = traceCam.nearZ;
    tcam.farZ         = traceCam.farZ;
    if (m_temporal)
    {
        m_temporal->SetCamera(tcam);
        // Same frame index as trace/resolve so all three see the same
        // sub-pixel jitter and read aligned full-res GBuffer texels.
        m_temporal->SetFrameIndex(frameIdx);
        m_temporal->SetRenderFrame(ctx.renderFrame);
    }
    // Advance for next frame now that all three sub-passes have been seeded.
    ++m_frameIndex;

    SSRUpsamplePass::Camera ucam{};
    ucam.invViewProj = traceCam.invViewProj;
    ucam.nearZ       = traceCam.nearZ;
    ucam.farZ        = traceCam.farZ;
    if (m_upsample) m_upsample->SetCamera(ucam);

    if (m_sceneColorPyr) m_sceneColorPyr->EnsureTexture(rw, rh);
    if (legacy && m_temporal) m_temporal->EnsureTextures(rw, rh);
    if (legacy && m_upsample) m_upsample->EnsureTexture(rw, rh);

    // ---- Phase 4.6: trace chain ----
    RHI::CommandList ssrCL = gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
    gfx.SetCommandListName(ssrCL, "SSR.trace");
    ssrCL.gfx = &gfx;
    GfxTrace::Label(ssrCL, "SSR.trace");
    if (colorLastCL.IsValid())
        gfx.AddCommandListDependency(ssrCL, colorLastCL);
    // Only the legacy march/denoise chain touches GBuffer_Depth (see the
    // `if (legacy)` block below); the pyramid-only path snapshots colour and
    // must NOT be pulled behind Hi-Z for nothing.
    if (legacy)
        chainDepth(ssrCL);

    const RHI::ResourceState depthState0    = graph.GetTextureState(ctx.depthHandle);
    const RHI::ResourceState normalState0   = graph.GetTextureState(ctx.normalHandle);
    const RHI::ResourceState surfaceState0  = graph.GetTextureState(ctx.surfaceHandle);
    const RHI::ResourceState velocityState0 = velocityTex
        ? graph.GetTextureState(ctx.velocityHandle)
        : RHI::ResourceState::SHADER_RESOURCE;
    const RHI::ResourceState traceDepthState0 = traceDepthTex
        ? graph.GetTextureState(ctx.traceDepthHandle)
        : RHI::ResourceState::SHADER_RESOURCE;

    // Address a depth-stencil texture's depth plane alone: the chain samples depth, never
    // stencil. A barrier on every plane from the depth plane's state (what the graph's
    // GetTextureState names) would drag the stencil plane along, out of the graph's book,
    // which tracks the planes apart. Every plane of any other texture, and under
    // -rgplaneoff.
    const bool planes = RG::RenderGraph::PlanesEnabled();
    auto planeOf = [planes](const RHI::Texture* t) {
        return planes && RHI::HasStencilPlane(t->desc.format) ? 0 : -1;
    };
    // A depth plane in DEPTH_READ_SRV is read as it is (that state holds both shader-read
    // bits), so it makes no round trip.
    auto asIs = [planes](const RHI::Texture* t, RHI::ResourceState s) {
        return s == RHI::ResourceState::SHADER_RESOURCE ||
               (planes && RHI::HasStencilPlane(t->desc.format) && s == RHI::ResourceState::DEPTH_READ_SRV);
    };
    auto toSR = [&](const RHI::Texture* t, RHI::ResourceState from) {
        if (!t) return;
        if (!asIs(t, from))
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, from, RHI::ResourceState::SHADER_RESOURCE, -1, -1, planeOf(t)), ssrCL);
    };
    // GBuffer inputs are only touched by the legacy march/denoise chain; the
    // pyramid-only path only snapshots the reflection-free Lighting MRT.
    if (legacy)
    {
        toSR(depthTex,      depthState0);
        toSR(normalTex,     normalState0);
        toSR(surfaceTex,    surfaceState0);
        toSR(velocityTex,   velocityState0);
        toSR(traceDepthTex, traceDepthState0);
    }

    // Per-sub-pass GPU timestamps (the SkyIBL / VolFog convention), so each sub-step shows
    // in the profiler panel instead of collapsing to a single "SSR" line.
    auto pBegin = [&](const char* name) -> uint32_t {
        return gfx.BeginGPUTimestamp(ssrCL, name);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(ssrCL, r);
    };

    // 1. Depth pyramid (internal chain leaves the texture in UAV).
    //    Built from the grass-free snapshot when available so blades don't
    //    block the Hi-Z march; falls back to the main depth otherwise.
    //    Legacy-only: the unified pass builds its own Hi-Z inside the graph.
    const RHI::Texture* hierTex = nullptr;
    if (legacy)
    {
        uint32_t r = pBegin("SSR.DepthHier");
        m_depthHier->Execute(ssrCL, gfx.GetTextureSRVGpuHandle(
            traceDepthTex ? *traceDepthTex : *depthTex));
        pEnd(r);
        hierTex = m_depthHier->GetTexture();
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            hierTex, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), ssrCL);
    }

    // 2. HDR snapshot + scene-color pyramid (trace consumes mip-chain).
    {
        uint32_t r = pBegin("SSR.SnapshotCopy");
        const RHI::Texture* snapTex = m_resolve->GetSnapshotTexture();
        const bool useReflectionSource =
            reflectionSourceTex && reflectionSourceTex->IsValid();
        const RHI::ResourceState reflectionSourceState = useReflectionSource
            ? graph.GetTextureState(ctx.reflectionSourceHandle)
            : RHI::ResourceState::UNDEFINED;

        if (useReflectionSource)
        {
            if (reflectionSourceState != RHI::ResourceState::COPY_SRC)
                gfx.PushBarrier(RHI::GPUBarrier::Image(
                    reflectionSourceTex, reflectionSourceState,
                    RHI::ResourceState::COPY_SRC), ssrCL);
        }
        else
        {
            ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                              "SSR snapshot SetHdrTextureState(COPY_SRC): HDR access in a push scope that is not the recording view's (depth=%d)",
                              static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
            gfx.SetHdrTextureState(RHI::ResourceState::COPY_SRC, ssrCL);
        }
        if (kSSREnableScratchAliasing && m_aliasHeap && !m_snapshotActive)
        {
            // Hand the shared heap bytes to snapshot (color is now inactive).
            // Do NOT reset the engine-tracked state: the debug layer tracks
            // resource state ACROSS aliasing barriers, so PreSnapshotCopy's
            // normal transition (from snapshot's real prior state) is what the
            // layer expects. Content is re-initialized by the full CopyHdrSceneTo
            // below (the aliasing init contract).
            // Pyramid-only frames leave the snapshot the ACTIVE resource, so
            // the barrier only fires on an actual owner switch (the resolve
            // step below flips ownership back to color on legacy frames).
            gfx.PushBarrier(RHI::GPUBarrier::Aliasing(
                m_resolve->GetColorTexture(), m_resolve->GetSnapshotTexture()), ssrCL);
        }
        if (kSSREnableScratchAliasing && m_aliasHeap)
            m_snapshotActive = true;
        m_resolve->PreSnapshotCopy(ssrCL);
        if (useReflectionSource)
            gfx.CopyTextureSubresource(
                *reflectionSourceTex, 0, 0, *snapTex, 0, 0, ssrCL);
        else
        {
            ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                              "SSR snapshot CopyHdrSceneTo: HDR access in a push scope that is not the recording view's (depth=%d)",
                              static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
            gfx.CopyHdrSceneTo(*snapTex, ssrCL);
        }
        m_resolve->PostSnapshotCopy(ssrCL);
        if (useReflectionSource &&
            reflectionSourceState != RHI::ResourceState::COPY_SRC)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                reflectionSourceTex, RHI::ResourceState::COPY_SRC,
                reflectionSourceState), ssrCL);
        }
        pEnd(r);
    }

    uint64_t pyramidSrv = m_resolve->GetSnapshotSrv();  // mip-0-only fallback
    if (m_sceneColorPyr)
    {
        uint32_t r = pBegin("SSR.SceneColorPyramid");
        // Execute leaves the pyramid parked in SHADER_RESOURCE (state contract in
        // SceneColorPyramidPass.h), so no external barriers here; RTReflectionTrace reads the
        // same texture NEXT frame.
        m_sceneColorPyr->Execute(ssrCL, m_resolve->GetSnapshotTexture());
        pEnd(r);
        if (uint64_t srv = m_sceneColorPyr->GetSrvHandle())
            pyramidSrv = srv;
    }

    // ---- Legacy march/denoise chain (water frames / unified fallback) ----
    if (legacy)
    {
    // 3. Trace: writes hit color + rayDirPDF + rayLength + rayClass.
    {
        uint32_t r = pBegin("SSR.Trace");
        m_trace->Execute(ssrCL,
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            m_depthHier->GetSrvHandle(),
            pyramidSrv,
            velocityTex ? gfx.GetTextureSRVGpuHandle(*velocityTex) : 0,
            ctx.skyRadianceSrv,
            ctx.skyRadianceMips,
            ctx.hasSkyRadiance);
        pEnd(r);
    }

    // All four trace outputs UAV -> SR for the resolve dispatch.
    auto uavToSR = [&](const RHI::Texture* t) {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            t, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), ssrCL);
    };
    uavToSR(m_trace->GetResultTexture());
    uavToSR(m_trace->GetRayDirPDFTexture());
    uavToSR(m_trace->GetRayLengthTexture());
    uavToSR(m_trace->GetRayClassTexture());
    m_trace->SetAllOutputsState(RHI::ResourceState::SHADER_RESOURCE);

    // All SSR sub-passes run at full render resolution (the reason: SSRPass::EnsureTexture).

    // 4. Resolve (spatial BRDF reweight + variance).
    {
        uint32_t r = pBegin("SSR.Resolve");
        if (kSSREnableScratchAliasing && m_aliasHeap)
        {
            // Hand the shared heap bytes back to color (snapshot finished being
            // read by the pyramid/trace above). No tracked-state reset (see the
            // snapshot barrier above); resolve's full-coverage UAV write
            // re-initializes the aliased memory.
            gfx.PushBarrier(RHI::GPUBarrier::Aliasing(
                m_resolve->GetSnapshotTexture(), m_resolve->GetColorTexture()), ssrCL);
            m_snapshotActive = false;   // color owns the heap again
        }
        m_resolve->Execute(ssrCL, rw, rh,
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            m_trace->GetResultSrv(),
            m_trace->GetRayDirPDFSrv(),
            m_trace->GetRayLengthSrv(),
            m_trace->GetRayClassSrv());
        pEnd(r);
    }

    // 5. Temporal (dual-reprojection history blend).
    if (m_temporal)
    {
        uint32_t r = pBegin("SSR.Temporal");
        m_temporal->Execute(ssrCL, rw, rh,
            m_resolve->GetColorSrv(),
            m_resolve->GetVarianceSrv(),
            m_resolve->GetReprojDepthSrv(),
            velocityTex ? gfx.GetTextureSRVGpuHandle(*velocityTex) : 0,
            gfx.GetTextureSRVGpuHandle(*depthTex));
        pEnd(r);
    }

    // 6. Upsample (variance-driven bilateral).
    if (m_upsample && m_temporal)
    {
        uint32_t r = pBegin("SSR.Upsample");
        m_upsample->Execute(ssrCL, rw, rh,
            m_temporal->GetColorSrv(),
            m_temporal->GetVarianceSrv(),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex));
        pEnd(r);
    }

    // Return the depth pyramid to UAV for next frame's mip 0 dispatch.
    if (hierTex)
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            hierTex, RHI::ResourceState::SHADER_RESOURCE,
            RHI::ResourceState::UNORDERED_ACCESS), ssrCL);
    }   // legacy march/denoise chain

    // Scene-color pyramid STAYS in SHADER_RESOURCE between frames (state contract in
    // SceneColorPyramidPass.h): next frame's Execute transitions it itself, and
    // RTReflectionTrace reads it before the SSR chain runs.

    // HDR end state: FullAdditive hands RENDERTARGET to the composite (which flips it
    // UAV -> SR); the composite-less modes park HDR in SHADER_RESOURCE themselves.
    ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                      "SSR legacy end-state SetHdrTextureState: HDR access in a push scope that is not the recording view's (depth=%d)",
                      static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
    gfx.SetHdrTextureState(mode == LegacyMode::FullAdditive
                               ? RHI::ResourceState::RENDERTARGET
                               : RHI::ResourceState::SHADER_RESOURCE, ssrCL);

    // Restore graph-tracked states so downstream passes see what they expect.
    if (legacy)
    {
        auto fromSR = [&](const RHI::Texture* t, RHI::ResourceState to) {
            if (!t) return;
            if (!asIs(t, to))
                gfx.PushBarrier(RHI::GPUBarrier::Image(
                    t, RHI::ResourceState::SHADER_RESOURCE, to, -1, -1, planeOf(t)), ssrCL);
        };
        fromSR(depthTex,      depthState0);
        fromSR(normalTex,     normalState0);
        fromSR(surfaceTex,    surfaceState0);
        fromSR(velocityTex,   velocityState0);
        fromSR(traceDepthTex, traceDepthState0);
    }

    // ---- Phase 4.7: composite (FullAdditive / water frames only) ----
    // Resolved SSR + Fresnel x envBRDF -> HDR additive, scaled by (1 - unified conf) so
    // pixels Lighting.ps already folded stay single-counted. FullFold skips this:
    // LightingPass folds the upsample output as its RT reflection input (t44) next frame
    // instead.
    if (mode != LegacyMode::FullAdditive) return ssrCL;
    if (!m_upsample || !m_composite) return ssrCL;

    const RHI::Texture* albedoTex = graph.GetPhysicalTexture(ctx.albedoHandle);
    const RHI::Texture* ssrTex    = m_upsample->GetColorTexture();
    ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                      "SSR composite GetHdrSceneUavGpuHandle: HDR access in a push scope that is not the recording view's (depth=%d)",
                      static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
    const uint64_t      hdrUav    = gfx.GetHdrSceneUavGpuHandle();
    if (!albedoTex || !ssrTex || !hdrUav) return ssrCL;

    SSRCompositePass::Camera ccam{};
    ccam.cameraPos = ctx.cameraPos;
    {
        XMMATRIX vp = XMLoadFloat4x4(&ctx.viewProj);
        XMStoreFloat4x4(&ccam.invViewProj, XMMatrixInverse(nullptr, vp));
    }
    m_composite->SetCamera(ccam);

    RHI::CommandList compCL = gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
    gfx.SetCommandListName(compCL, "SSR.composite");
    compCL.gfx = &gfx;
    GfxTrace::Label(compCL, "SSR.composite");
    if (colorLastCL.IsValid())
        gfx.AddCommandListDependency(compCL, colorLastCL);
    // Reached only in FullAdditive, and it always round-trips depth below.
    chainDepth(compCL);

    const RHI::ResourceState albedoState0   = graph.GetTextureState(ctx.albedoHandle);
    const RHI::ResourceState normalState0c  = graph.GetTextureState(ctx.normalHandle);
    const RHI::ResourceState surfaceState0c = graph.GetTextureState(ctx.surfaceHandle);
    const RHI::ResourceState depthState0c   = graph.GetTextureState(ctx.depthHandle);
    // The depth texture's depth plane alone, as in the trace chain.
    const bool planesC = RG::RenderGraph::PlanesEnabled();
    auto planeOfC = [planesC](const RHI::Texture* t) {
        return planesC && RHI::HasStencilPlane(t->desc.format) ? 0 : -1;
    };
    // As in the trace chain: a depth plane in DEPTH_READ_SRV makes no round trip.
    auto asIsC = [planesC](const RHI::Texture* t, RHI::ResourceState s) {
        return s == RHI::ResourceState::SHADER_RESOURCE ||
               (planesC && RHI::HasStencilPlane(t->desc.format) && s == RHI::ResourceState::DEPTH_READ_SRV);
    };
    auto toSR_c = [&](const RHI::Texture* t, RHI::ResourceState from) {
        if (!asIsC(t, from))
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, from, RHI::ResourceState::SHADER_RESOURCE, -1, -1, planeOfC(t)), compCL);
    };
    toSR_c(albedoTex,  albedoState0);
    toSR_c(normalTex,  normalState0c);
    toSR_c(surfaceTex, surfaceState0c);
    toSR_c(depthTex,   depthState0c);

    ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                      "SSR composite SetHdrTextureState(UAV): HDR access in a push scope that is not the recording view's (depth=%d)",
                      static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
    gfx.SetHdrTextureState(RHI::ResourceState::UNORDERED_ACCESS, compCL);

    // Unified overlap guard: (1 - rtConf) modulation against the same-frame
    // RTReflectionPass output (0 = no unified pass this frame -> no modulation).
    m_composite->SetUnifiedConfSrv(ctx.unifiedConfSrv);

    {
        uint32_t r = gfx.BeginGPUTimestamp(compCL, "SSR.Composite");
        m_composite->Execute(compCL, rw, rh,
            gfx.GetTextureSRVGpuHandle(*albedoTex),
            gfx.GetTextureSRVGpuHandle(*normalTex),
            gfx.GetTextureSRVGpuHandle(*surfaceTex),
            gfx.GetTextureSRVGpuHandle(*depthTex),
            gfx.GetTextureSRVGpuHandle(*ssrTex),
            hdrUav,
            ctx.brdfLutSrv);
        gfx.EndGPUTimestamp(compCL, r);
    }

    ENGINE_CHECK_ONCE(HdrIsRecordingViews(gfx),
                      "SSR composite SetHdrTextureState(SRV): HDR access in a push scope that is not the recording view's (depth=%d)",
                      static_cast<GraphicsDX12&>(gfx).HdrScopeDepth());
    gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE, compCL);

    auto fromSR_c = [&](const RHI::Texture* t, RHI::ResourceState to) {
        if (!asIsC(t, to))
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                t, RHI::ResourceState::SHADER_RESOURCE, to, -1, -1, planeOfC(t)), compCL);
    };
    fromSR_c(albedoTex,  albedoState0);
    fromSR_c(normalTex,  normalState0c);
    fromSR_c(surfaceTex, surfaceState0c);
    fromSR_c(depthTex,   depthState0c);
    return compCL;
}
