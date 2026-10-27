#include "RenderGraph/RenderPass/SSGIPass.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/DDGIVolumeManager.h"   // DDGI::kMaxVolumes (register-array width)
#include "Graphics/NrdDenoiser.h"         // REBLUR_DIFFUSE
#include "RenderGraph/PassTextureNames.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/CommandLine.h"

#include <vector>
#include <string>
#include <iterator>
#include <cstring>
#include <algorithm>

namespace
{
// Shader compiles go through the shared disk cache (ShaderDiskCache: the
// .ishdr cache in shaders/shader_cache_dxil/, keyed on path + entry + stage +
// defines + compile flags, invalidated by the mtime of the source or any of its
// transitive #includes); an uncached compile is a full DXC run on every boot.
std::vector<uint8_t> CompileShaderFile(const std::string& path, const std::string& entry)
{
    return ShaderDiskCache::LoadOrCompile(path, entry, RHI::ShaderStage::CS, {}, "SSGIPass");
}

// Root parameter indices - must stay in sync with shaders/SSGICommon.hlsli,
// the single source of truth for this layout.
constexpr RHI::Slots::CmpParam kCB                { 0 };   // b0
constexpr RHI::Slots::CmpParam kSRV_Depth         { 1 };   // t1
constexpr RHI::Slots::CmpParam kSRV_Normal        { 2 };   // t2
constexpr RHI::Slots::CmpParam kSRV_Velocity      { 3 };   // t3
constexpr RHI::Slots::CmpParam kSRV_DepthHier     { 4 };   // t4
constexpr RHI::Slots::CmpParam kSRV_PrevHDR       { 5 };   // t5
constexpr RHI::Slots::CmpParam kSRV_DiffDenoised  { 6 };   // t6
constexpr RHI::Slots::CmpParam kSRV_RawCoverage   { 7 };   // t7
constexpr RHI::Slots::CmpParam kSRV_DDGIVolumes   { 8 };   // t8
constexpr RHI::Slots::CmpParam kSRV_DDGIIrr       { 9 };   // t0 space4, kMaxVolumes wide
constexpr RHI::Slots::CmpParam kSRV_DDGIDepth     { 10 };  // t0 space5
constexpr RHI::Slots::CmpParam kSRV_DDGIProbe     { 11 };  // t0 space6
constexpr RHI::Slots::CmpParam kUAV_Raw           { 12 };  // u0
constexpr RHI::Slots::CmpParam kUAV_NormalRough   { 13 };  // u1
constexpr RHI::Slots::CmpParam kUAV_ViewZ         { 14 };  // u2
constexpr RHI::Slots::CmpParam kUAV_MV            { 15 };  // u3
constexpr RHI::Slots::CmpParam kUAV_DiffIn        { 16 };  // u4
constexpr RHI::Slots::CmpParam kUAV_Result        { 17 };  // u5
constexpr RHI::Slots::CmpParam kSRV_CovHistory    { 18 };  // t9  (previous frame)
constexpr RHI::Slots::CmpParam kUAV_CovHistory    { 19 };  // u6  (this frame)
constexpr RHI::Slots::CmpParam kSRV_PrevDepth     { 20 };  // t10 (previous frame)
constexpr RHI::Slots::CmpParam kUAV_DepthHistory  { 21 };  // u7  (this frame)
constexpr RHI::Slots::CmpParam kSRV_ReservoirHistoryDirection { 22 }; // t11
constexpr RHI::Slots::CmpParam kSRV_ReservoirHistoryRadiance  { 23 }; // t12
constexpr RHI::Slots::CmpParam kSRV_ReservoirHistoryAux       { 24 }; // t13
constexpr RHI::Slots::CmpParam kSRV_ReservoirHistoryMeta      { 25 }; // t14
constexpr RHI::Slots::CmpParam kSRV_ReservoirTemporalDirection { 26 }; // t15
constexpr RHI::Slots::CmpParam kSRV_ReservoirTemporalRadiance  { 27 }; // t16
constexpr RHI::Slots::CmpParam kSRV_ReservoirTemporalAux       { 28 }; // t17
constexpr RHI::Slots::CmpParam kSRV_ReservoirTemporalMeta      { 29 }; // t18
constexpr RHI::Slots::CmpParam kUAV_ReservoirTemporalDirection { 30 }; // u8
constexpr RHI::Slots::CmpParam kUAV_ReservoirTemporalRadiance  { 31 }; // u9
constexpr RHI::Slots::CmpParam kUAV_ReservoirTemporalAux       { 32 }; // u10
constexpr RHI::Slots::CmpParam kUAV_ReservoirTemporalMeta      { 33 }; // u11
constexpr RHI::Slots::CmpParam kUAV_ReservoirHistoryDirection  { 34 }; // u12
constexpr RHI::Slots::CmpParam kUAV_ReservoirHistoryRadiance   { 35 }; // u13
constexpr RHI::Slots::CmpParam kUAV_ReservoirHistoryAux        { 36 }; // u14
constexpr RHI::Slots::CmpParam kUAV_ReservoirHistoryMeta       { 37 }; // u15
constexpr uint32_t kNumRootParams     = 38;

void TransitionTex(IGraphicsDevice& gfx, const RHI::Texture& tex,
                   RHI::ResourceState& state, RHI::ResourceState to, RHI::CommandList cl)
{
    if (state == to || !tex.IsValid()) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}
} // namespace

SSGIPass::SSGIPass(RG::RGTextureHandle normal,
                   RG::RGTextureHandle depth,
                   RG::RGTextureHandle velocity,
                   RG::RGTextureHandle ssrTraceDepth)
    : m_normalH(normal), m_depthH(depth), m_velocityH(velocity),
      m_ssrTraceDepthH(ssrTraceDepth)
{
}

void SSGIPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_resultH = d.Import(RG::PassTex::kSSGIResult, { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true },
                         { &m_resultImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
    m_hiZH    = d.Import(RG::PassTex::kSSGIHiZ, { RHI::Format::R16G16_FLOAT, 0, 0, false, true },
                         { &m_hiZOut, RG::ImportEntry::Stale, RG::ImportTracking::Owner });
}

void SSGIPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_normalH);
    b.ReadSRV(m_depthH);
    b.ReadSRV(m_velocityH);
    b.ReadSRV(m_ssrTraceDepthH);
    // The march reuses last frame's SSR scene pyramid; it writes its result (the
    // graph brings it to UAV) and its Hi-Z (barriered here).
    b.ReadHistory(b.GetTexture(RG::PassTex::kSSRScenePyramid));
    b.WriteUAV(m_resultH);
    b.WriteUAV(m_hiZH);
}

void SSGIPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    // Unlike RTAOPass / RTShadowPass / RTReflectionPass this pass needs no DXR: it
    // is purely screen-space and works on any SM6.6 device, so it is also the GI
    // improvement where the RT passes are unavailable.
    if (!BuildRootSignature(gfx)) return;
    if (!CreatePipelines(gfx))    return;
    // The constant buffer is mandatory, and its absence hides at every layer:
    // FrameCB::Current() returns nullptr when uncreated (the write site skips a null
    // slot), SetComputeRootCBV early-returns on an invalid buffer (b0 unbound, no
    // D3D12 validation error), the shaders then read screenW/screenH as 0 and exit
    // at their bounds check, and the CPU still records the dispatches. Fail loudly.
    if (!m_cb.Create(gfx, "SSGIPass.CB"))
    {
        LOG_ERROR("SSGIPass: constant buffer creation failed — pass disabled");
        return;
    }
    m_ws->hiZ = std::make_unique<SSRDepthHierarchyPass>();
    m_ws->hiZ->Init(gfx);
    m_ready = true;
    LOG_INFO("SSGIPass: ready (screen-space diffuse GI, REBLUR_DIFFUSE)");
}

void SSGIPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_ready) return;
    gfx.DestroyPipelineState(m_tracePSO);
    gfx.DestroyPipelineState(m_reservoirSpatialPSO);
    gfx.DestroyPipelineState(m_resolvePSO);
    // Rebuild the root signature too: after a layout edit a hot reload would
    // otherwise feed the new shader to the old signature, CreateComputePipelineState
    // fails, and the pass disables itself for the rest of the session.
    gfx.DestroyPipelineLayout(m_layout);
    if (m_mainWs.hiZ) m_mainWs.hiZ->ReloadShaders(gfx);
    if (!BuildRootSignature(gfx))
    {
        LOG_ERROR("SSGIPass: root signature rebuild failed — pass disabled");
        m_ready = false;
        return;
    }
    if (!CreatePipelines(gfx))
    {
        LOG_ERROR("SSGIPass: shader reload failed — pass disabled for this session");
        m_ready = false;
        return;
    }
    if (m_hist)
    {
        m_hist->reset = true;
        m_hist->reservoirValid = false;
    }
}

void SSGIPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    if (view.Slot() == 0)
        m_ws = &m_mainWs;
    else
    {
        ViewWork& work = view.Data<ViewWork>(this);
        if (!work.owner)
        {
            // First bound on the main thread (ServiceViews): the Hi-Z builder's pipelines are made here.
            work.owner = this;
            if (m_gfx && m_ready)
            {
                work.ws.hiZ = std::make_unique<SSRDepthHierarchyPass>();
                work.ws.hiZ->Init(*m_gfx);
            }
        }
        m_ws = &work.ws;
    }
    SyncImports();
}

void SSGIPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &ws.raw, &ws.normalRough, &ws.viewZ, &ws.mv, &ws.diffIn, &ws.diffOut, &ws.result,
                             &ws.reservoirTemporal.direction, &ws.reservoirTemporal.radiance,
                             &ws.reservoirTemporal.aux, &ws.reservoirTemporal.meta })
        if (t->IsValid()) gfx.DestroyTexture(*t);
    if (ws.hiZ) ws.hiZ->ReleaseTextures();
    if (owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd), RHI::kFramesInFlight + 1u);
    ws = WorkingSet{};
}

void SSGIPass::SyncImports()
{
    m_resultImport = m_ws->result;
    const RHI::Texture* hiZ = m_ws->hiZ ? m_ws->hiZ->GetTexture() : nullptr;
    m_hiZOut = hiZ ? *hiZ : RHI::Texture{};
}

void SSGIPass::RetireNrd()
{
    if (m_ws->nrd) m_retiredNrd.emplace_back(std::move(m_ws->nrd), RHI::kFramesInFlight + 1u);
}

// The frame boundary (the main view bound): the reservoir switch, the main view's working set, the retired
// NRD pools whose frames have drained, and the imports. (The Hi-Z builder is made in Init, after the first
// Compile names the imports, so the graph reads copies.)
void SSGIPass::OnService(RG::ServiceContext&)
{
    SetReservoirEnabled(m_request.reservoir);
    EnsureResources(m_request.wanted, m_request.hiZOnly);
    for (size_t i = 0; i < m_retiredNrd.size();)
    {
        if (--m_retiredNrd[i].second == 0)
            m_retiredNrd.erase(m_retiredNrd.begin() + static_cast<ptrdiff_t>(i));
        else
            ++i;
    }
    SyncImports();
}

void SSGIPass::EnsureViewResources(uint32_t w, uint32_t h)
{
    if (m_ws == &m_mainWs || !m_gfx || !m_tracePSO.IsValid() || !m_reservoirSpatialPSO.IsValid()) return;
    WorkingSet& ws = *m_ws;
    ws.resourcesWanted = m_mainWs.resourcesWanted;
    ws.hiZWanted       = m_mainWs.hiZWanted;
    if (ws.hiZWanted)
        BuildWorkingSet(*m_gfx, w, h, /*mayStall=*/false);
    if (!ws.resourcesWanted && (ws.result.IsValid() || ws.nrd))
    {
        DestroyResolutionResources(*m_gfx);
        RetireNrd();
        ws.w = ws.h = 0;
        if (m_hist) m_hist->depthValid = false;
    }
    SyncImports();
}

void SSGIPass::History::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &covHistory[0], &covHistory[1], &depthHistory })
    {
        if (t->IsValid()) gfx.DestroyTexture(*t);
        *t = {};
    }
    for (ReservoirSet& r : reservoirHistory)
    {
        for (RHI::Texture* t : { &r.direction, &r.radiance, &r.aux, &r.meta })
            if (t->IsValid()) gfx.DestroyTexture(*t);
        r = {};
    }
    depthValid = reservoirValid = false;
}

bool SSGIPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    // Every range explicit (register, count, space) - the DDGI arrays are
    // kMaxVolumes wide; a 1-descriptor table there would read out of bounds
    // for every volume > 0 with no debug-layer error.
    RHI::PipelineLayoutBuilder b("SSGI");
    b.RootCBV(kCB, 0)
     .Table(kSRV_Depth,        { RHI::SrvRange(1) })
     .Table(kSRV_Normal,       { RHI::SrvRange(2) })
     .Table(kSRV_Velocity,     { RHI::SrvRange(3) })
     .Table(kSRV_DepthHier,    { RHI::SrvRange(4) })
     .Table(kSRV_PrevHDR,      { RHI::SrvRange(5) })
     .Table(kSRV_DiffDenoised, { RHI::SrvRange(6) })
     .Table(kSRV_RawCoverage,  { RHI::SrvRange(7) })
     .Table(kSRV_DDGIVolumes,  { RHI::SrvRange(8) })
     .Table(kSRV_DDGIIrr,      { RHI::SrvRange(0, DDGI::kMaxVolumes, 4) })
     .Table(kSRV_DDGIDepth,    { RHI::SrvRange(0, DDGI::kMaxVolumes, 5) })
     .Table(kSRV_DDGIProbe,    { RHI::SrvRange(0, DDGI::kMaxVolumes, 6) })
     .Table(kUAV_Raw,          { RHI::UavRange(0) })
     .Table(kUAV_NormalRough,  { RHI::UavRange(1) })
     .Table(kUAV_ViewZ,        { RHI::UavRange(2) })
     .Table(kUAV_MV,           { RHI::UavRange(3) })
     .Table(kUAV_DiffIn,       { RHI::UavRange(4) })
     .Table(kUAV_Result,       { RHI::UavRange(5) })
     .Table(kSRV_CovHistory,   { RHI::SrvRange(9) })
     .Table(kUAV_CovHistory,   { RHI::UavRange(6) })
     .Table(kSRV_PrevDepth,    { RHI::SrvRange(10) })
     .Table(kUAV_DepthHistory, { RHI::UavRange(7) })
     .Table(kSRV_ReservoirHistoryDirection,  { RHI::SrvRange(11) })
     .Table(kSRV_ReservoirHistoryRadiance,   { RHI::SrvRange(12) })
     .Table(kSRV_ReservoirHistoryAux,        { RHI::SrvRange(13) })
     .Table(kSRV_ReservoirHistoryMeta,       { RHI::SrvRange(14) })
     .Table(kSRV_ReservoirTemporalDirection, { RHI::SrvRange(15) })
     .Table(kSRV_ReservoirTemporalRadiance,  { RHI::SrvRange(16) })
     .Table(kSRV_ReservoirTemporalAux,       { RHI::SrvRange(17) })
     .Table(kSRV_ReservoirTemporalMeta,      { RHI::SrvRange(18) })
     .Table(kUAV_ReservoirTemporalDirection, { RHI::UavRange(8) })
     .Table(kUAV_ReservoirTemporalRadiance,  { RHI::UavRange(9) })
     .Table(kUAV_ReservoirTemporalAux,       { RHI::UavRange(10) })
     .Table(kUAV_ReservoirTemporalMeta,      { RHI::UavRange(11) })
     .Table(kUAV_ReservoirHistoryDirection,  { RHI::UavRange(12) })
     .Table(kUAV_ReservoirHistoryRadiance,   { RHI::UavRange(13) })
     .Table(kUAV_ReservoirHistoryAux,        { RHI::UavRange(14) })
     .Table(kUAV_ReservoirHistoryMeta,       { RHI::UavRange(15) });
    // s0 - linear CLAMP: the scene-colour pyramid and the depth hierarchy. WRAP
    // would fetch radiance from the opposite screen edge for any ray that exits the
    // frame - a wrong-but-plausible sample rather than a rejected one. MaxLOD stays
    // FLT_MAX (SamplerDesc's default): a zero MaxLOD clamps every SampleLevel to
    // mip 0.
    RHI::SamplerDesc linearClamp{};
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = RHI::TextureAddressMode::CLAMP;
    linearClamp.maxAnisotropy = 1;
    b.Sampler(linearClamp, 0);
    // s1 - linear WRAP: the DDGI octahedral atlases (their border ring makes
    // the addressing mode moot in practice, but this matches every other DDGI
    // consumer and keeps the sampling bit-identical to them).
    RHI::SamplerDesc linearWrap{};
    linearWrap.maxAnisotropy = 1;
    b.Sampler(linearWrap, 1);
    // HEAP_DIRECTLY_INDEXED like the shared graphics / compute signatures, DDGI,
    // RTR and the RTDI shell, so whether a shader may open ResourceDescriptorHeap[]
    // does not depend on which pass binds it.
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("SSGIPass: pipeline layout failed");
        return false;
    }
    return true;
}

bool SSGIPass::CreatePipelines(IGraphicsDevice& gfx)
{
    auto buildCS = [&](const std::string& path, RHI::PipelineState& outPSO) -> bool
    {
        const auto dxil = CompileShaderFile(path, "main");
        if (dxil.empty()) return false;
        RHI::Shader cs;
        RHI::PipelineStateDesc pd{};
        pd.cs = &cs; pd.layout = &m_layout;
        const bool ok = gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs) &&
                        gfx.CreatePipelineState(pd, outPSO);
        gfx.DestroyShader(cs);   // the PSO keeps its own copy of the DXIL
        if (!ok) LOG_ERROR("SSGIPass: pipeline failed for %s", path.c_str());
        return ok;
    };
    if (!buildCS("shaders/SSGITrace.cs.hlsl",   m_tracePSO))   return false;
    if (!buildCS("shaders/SSGIReservoirSpatial.cs.hlsl",
                 m_reservoirSpatialPSO)) return false;
    if (!buildCS("shaders/SSGIResolve.cs.hlsl", m_resolvePSO)) return false;
    return true;
}

void SSGIPass::DestroyReservoirResources(IGraphicsDevice& gfx)
{
    auto destroyReservoir = [&](ReservoirSet& r)
    {
        for (RHI::Texture* t : { &r.direction, &r.radiance, &r.aux, &r.meta })
            if (t->IsValid()) gfx.DestroyTexture(*t);
        r = {};
    };
    destroyReservoir(m_ws->reservoirTemporal);
    if (!m_hist) return;
    destroyReservoir(m_hist->reservoirHistory[0]);
    destroyReservoir(m_hist->reservoirHistory[1]);
    m_hist->reservoirWrite = 0;
    m_hist->reservoirValid = false;
}

void SSGIPass::DestroyResolutionResources(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &m_ws->raw, &m_ws->normalRough, &m_ws->viewZ, &m_ws->mv,
                             &m_ws->diffIn, &m_ws->diffOut, &m_ws->result })
        if (t->IsValid()) gfx.DestroyTexture(*t);
    // The view's coverage / depth histories, after the working set.
    if (m_hist)
        for (RHI::Texture* t : { &m_hist->covHistory[0], &m_hist->covHistory[1], &m_hist->depthHistory })
            if (t->IsValid()) gfx.DestroyTexture(*t);
    DestroyReservoirResources(gfx);
    m_ws->raw = {}; m_ws->normalRough = {}; m_ws->viewZ = {}; m_ws->mv = {};
    m_ws->diffIn = {}; m_ws->diffOut = {}; m_ws->result = {};
    if (m_hist)
    {
        m_hist->covHistory[0] = {}; m_hist->covHistory[1] = {};
        m_hist->depthHistory = {};
        m_hist->depthValid = false;
    }
}

void SSGIPass::EnsureResources(bool wanted, bool hiZOnlyWanted)
{
    m_ws->resourcesWanted = wanted;
    m_ws->hiZWanted = wanted || hiZOnlyWanted;
    if (!m_gfx || !m_tracePSO.IsValid() || !m_reservoirSpatialPSO.IsValid()) return;

    if (wanted || hiZOnlyWanted)
    {
        OnCompile(*m_gfx);   // idempotent - OnCompile early-outs when sized
        if (wanted) return;
    }
    if (!m_ws->result.IsValid() && !m_ws->nrd) return;   // already released

    // NrdDenoiser tears its pool down immediately (no deferral), so drain the
    // GPU first - the documented Resize convention. A settings toggle is far
    // rarer than a resize; the one-off stall is fine.
    m_gfx->WaitIdleAndReleaseDeferred();
    DestroyResolutionResources(*m_gfx);
    m_ws->nrd.reset();
    m_ws->w = m_ws->h = 0;
    if (m_hist) m_hist->depthValid = false;
    LOG_INFO("SSGIPass: working set + NRD pool released (SSGI switched off)");
}

void SSGIPass::OnCompile(IGraphicsDevice& gfx)
{
    BuildWorkingSet(gfx, gfx.GetRenderWidth(), gfx.GetRenderHeight(), /*mayStall=*/true);
    SyncImports();
}

void SSGIPass::BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall)
{
    if (!m_tracePSO.IsValid()) return;
    // Enable gate: boot and resolution-change Compiles land here for EVERY
    // registered pass. A disabled SSGI must not pay for a full-res working set
    // plus an NRD pool that nothing will read.
    if (w == 0 || h == 0) return;
    if (m_ws->hiZWanted && m_ws->hiZ)
    {
        const bool recreated=m_ws->hiZ->GetWidth()!=w || m_ws->hiZ->GetHeight()!=h || !m_ws->hiZ->GetTexture()->IsValid();
        m_ws->hiZ->EnsureTexture(w,h);
        if(recreated) m_ws->hiZState=RHI::ResourceState::UNORDERED_ACCESS;
    }
    if (!m_ws->resourcesWanted) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "SSGIPass: no view bound (PassSet::BindView) - no SSGI"))
        return;
    History& hist = *m_hist;
    const bool coreReady = w == m_ws->w && h == m_ws->h && m_ws->result.IsValid() &&
                           hist.depthHistory.IsValid() && m_ws->nrd && m_ws->nrd->IsReady();
    const bool reservoirReady = m_ws->reservoirTemporal.IsValid() &&
                                 hist.reservoirHistory[0].IsValid() &&
                                 hist.reservoirHistory[1].IsValid();
    auto reservoirHasAnyTexture = [](const ReservoirSet& r)
    {
        return r.direction.IsValid() || r.radiance.IsValid() ||
               r.aux.IsValid() || r.meta.IsValid();
    };
    const bool reservoirAllocated = reservoirHasAnyTexture(m_ws->reservoirTemporal) ||
                                    reservoirHasAnyTexture(hist.reservoirHistory[0]) ||
                                    reservoirHasAnyTexture(hist.reservoirHistory[1]);
    if (coreReady && !m_reservoirEnabled)
    {
        // The optional history is about 24 B x 3 sets per half-res pixel (~36 MiB at
        // 1080p). Release it on the same safe frame-boundary path used for enable
        // rather than keep that cost in non-reservoir mode.
        if (reservoirAllocated)
        {
            if (mayStall) gfx.WaitIdleAndReleaseDeferred();
            DestroyReservoirResources(gfx);
        }
        return;
    }
    if (coreReady && reservoirReady) return;
    // Enabling the reservoir after non-reservoir SSGI has run needs a safe
    // frame-boundary rebuild. ServiceEnableGatedResources calls us at exactly that
    // point; the one-off stall beats destroying textures an in-flight frame still
    // references.
    if (coreReady && m_reservoirEnabled && !reservoirReady && mayStall)
        gfx.WaitIdleAndReleaseDeferred();

    DestroyResolutionResources(gfx);
    m_ws->w = w; m_ws->h = h;
    // Round UP: an odd render height must not lose its last row of traced
    // samples, and the resolve clamps its half-res reads anyway.
    m_ws->tw = (w + 1u) / 2u;
    m_ws->th = (h + 1u) / 2u;
    hist.reset = true;
    // Hi-Z stays FULL resolution. The march walks it in full-res pixels on
    // purpose (see SSGICommon.hlsli) - halving it would halve the step density
    // along the same world ray and start stepping over thin geometry.
    if (m_ws->hiZ)
    {
        const bool recreateHiZ = m_ws->hiZ->GetWidth() != w || m_ws->hiZ->GetHeight() != h ||
                                 !m_ws->hiZ->GetTexture()->IsValid();
        m_ws->hiZ->EnsureTexture(w, h);
        if (recreateHiZ)
            m_ws->hiZState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    // `half` selects the trace/denoiser working set; only the final result is
    // full-res, because Lighting.ps reads it per full-res pixel.
    auto make = [&](RHI::Format fmt, RHI::Texture& tex, RHI::ResourceState& st,
                    const char* tag, bool half = true)
    {
        RHI::TextureDesc td{};
        td.width = half ? m_ws->tw : w; td.height = half ? m_ws->th : h;
        td.depth = 1; td.mipLevels = 1;
        td.format     = fmt;
        // debugName is not cosmetic: the RenderDoc MCP bridge cannot report compute
        // UAV outputs or read pipeline state and has no name -> ResourceId lookup, so
        // the qrenderdoc Texture Viewer filtered by name is the only way to inspect
        // what this pass wrote.
        td.debugName = tag;
        // UNORDERED_ACCESS on every one, including IN_MV (REBLUR_DIFFUSE writes it;
        // see SSGIPass::mv).
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        if (!gfx.CreateTexture(td, tex, nullptr))
            LOG_ERROR("SSGIPass: %s texture creation failed", tag);
        st = RHI::ResourceState::SHADER_RESOURCE;
    };
    make(RHI::Format::R16_FLOAT,          m_ws->raw,         m_ws->rawState,         "SSGIPass.RawCoverage");
    make(RHI::Format::R10G10B10A2_UNORM,  m_ws->normalRough, m_ws->normalRoughState, "SSGIPass.NrdNormalRough");
    make(RHI::Format::R32_FLOAT,          m_ws->viewZ,       m_ws->viewZState,       "SSGIPass.NrdViewZ");
    make(RHI::Format::R16G16_FLOAT,       m_ws->mv,          m_ws->mvState,          "SSGIPass.NrdMV");
    make(RHI::Format::R16G16B16A16_FLOAT, m_ws->diffIn,      m_ws->diffInState,      "SSGIPass.NrdDiffIn");
    make(RHI::Format::R16G16B16A16_FLOAT, m_ws->diffOut,     m_ws->diffOutState,     "SSGIPass.NrdDiffOut");
    // Result zero-initialised by creation: zero is rgb = 0 and coverage = 0,
    // exactly "no SSGI contribution", so if an early-out skips a frame while
    // LightCB already points here, Lighting.ps folds an exact no-op rather than
    // uninitialised VRAM.
    make(RHI::Format::R16G16B16A16_FLOAT, m_ws->result,      m_ws->resultState,
         "SSGIPass.Result(FOLDED BY LIGHTING)", /*half=*/false);
    // Full res so it can be reprojected with the full-res motion vectors the
    // resolve already reads; 2 MB at 1080p, not worth halving.
    make(RHI::Format::R16_FLOAT, hist.covHistory[0], hist.covHistoryState[0],
         "SSGIPass.CovHistory0", /*half=*/false);
    make(RHI::Format::R16_FLOAT, hist.covHistory[1], hist.covHistoryState[1],
         "SSGIPass.CovHistory1", /*half=*/false);
    make(RHI::Format::R32_FLOAT, hist.depthHistory, hist.depthHistoryState,
         "SSGIPass.DepthHistory", /*half=*/false);

    auto makeReservoir = [&](ReservoirSet& r, const char* suffix)
    {
        const std::string base = std::string("SSGIPass.Reservoir.") + suffix;
        make(RHI::Format::R16G16B16A16_FLOAT, r.direction, r.directionState,
             (base + ".Direction").c_str());
        make(RHI::Format::R16G16B16A16_FLOAT, r.radiance, r.radianceState,
             (base + ".Radiance").c_str());
        make(RHI::Format::R16G16_FLOAT, r.aux, r.auxState,
             (base + ".Aux").c_str());
        make(RHI::Format::R32_UINT, r.meta, r.metaState,
             (base + ".Meta").c_str());
    };
    if (m_reservoirEnabled)
    {
        makeReservoir(m_ws->reservoirTemporal, "Temporal");
        makeReservoir(hist.reservoirHistory[0], "History0");
        makeReservoir(hist.reservoirHistory[1], "History1");
    }
    hist.covWrite = 0;
    hist.depthValid = false;
    hist.reservoirWrite = 0;
    hist.reservoirValid = false;

    // A view rebuilds inside its segment, where its old pool may still be read by a frame in flight: a fresh
    // instance, the old one retired.
    if (!mayStall) RetireNrd();
    if (!m_ws->nrd)
    {
        m_ws->nrd = std::make_unique<NrdDenoiser>();
        m_ws->nrd->SetKind(NrdDenoiser::Kind::ReblurDiffuse);
    }
    // REBLUR runs at the TRACE resolution: it denoises the traced signal, and
    // denoising a bilinear upsample of it would be slower and worse (the upsample
    // correlates neighbouring pixels, which a spatial denoiser's variance estimate
    // assumes they are not).
    if (!m_ws->nrd->Resize(gfx, m_ws->tw, m_ws->th))
    {
        LOG_ERROR("SSGIPass: NRD REBLUR_DIFFUSE resize failed — pass inert");
        m_ws->nrd.reset();
        return;
    }
}

uint64_t SSGIPass::GetUpcomingOutputSrv() const
{
    if (!m_ready || !m_gfx || !m_ws->result.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_ws->result);
}

RHI::CommandList SSGIPass::Execute(RHI::CommandList cl)
{
    // Every early return below records why (execBailReason), and
    // lastDispatchedPixels is cleared on entry: a diagnostic set only on success
    // and never cleared reports a sticky "ran once" every frame - worse than none.
    m_ws->lastDispatchedPixels = 0;
    auto bail = [&](const char* why) { m_ws->execBailReason = why; return cl; };
    m_ws->execBailReason = nullptr;

    if (!m_ready)          return bail("not ready");
    if (!m_ws->active)         return bail("m_ws->active false (graph should not have called us)");

    if (debugMode != m_lastDebugMode)
    {
        m_lastDebugMode = debugMode;
        if (m_hist)
        {
            m_hist->reset = true;
            m_hist->reservoirValid = false;
        }
    }
    // Calibration mode 1 deliberately bypasses every trace. The binding /
    // coverage / near-field debug views (2..4) keep the selected sampling path, so
    // a debug view never silently switches reservoir mode off and its noise stays
    // inspectable.
    const bool useReservoirThisFrame = m_reservoirEnabled && debugMode != 1u;

    IGraphicsDevice& gfx = cl.GetDevice();

    // GPU timestamps per stage, so the trace, the denoiser and the resolve are
    // not one opaque number. Names follow the DDGI. / RTR. convention so they
    // group in the profiler.
    struct StageTimer
    {
        IGraphicsDevice& g; RHI::CommandList c; uint32_t r;
        StageTimer(IGraphicsDevice& gg, RHI::CommandList cc, const char* n)
            : g(gg), c(cc), r(gg.BeginGPUTimestamp(cc, n)) {}
        ~StageTimer() { g.EndGPUTimestamp(c, r); }
    };


    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex    = ctx.GetTexture(m_depthH);
    const RHI::Texture* normalTex   = ctx.GetTexture(m_normalH);
    const RHI::Texture* velocityTex = ctx.GetTexture(m_velocityH);
    if (!depthTex)    return bail("graph depth texture null");
    if (!normalTex)   return bail("graph normal texture null");
    if (!velocityTex) return bail("graph velocity texture null");
    const uint64_t depthSrv    = gfx.GetTextureSRVGpuHandle(*depthTex);
    const uint64_t normalSrv   = gfx.GetTextureSRVGpuHandle(*normalTex);
    const uint64_t velocitySrv = gfx.GetTextureSRVGpuHandle(*velocityTex);
    if (!depthSrv)    return bail("depth SRV 0");
    if (!normalSrv)   return bail("normal SRV 0");
    if (!velocitySrv) return bail("velocity SRV 0");

    // Grass-free depth for the hierarchy; fall back to the main depth if the
    // graph has no separate trace-depth this frame (grass then just acts as an
    // occluder - degraded, not wrong).
    const RHI::Texture* hizSrc = ctx.GetTexture(m_ssrTraceDepthH);
    if (!hizSrc) hizSrc = depthTex;
    if (!m_ws->hiZ)  return bail("no Hi-Z builder");
    if (!hizSrc) return bail("no Hi-Z source depth");

    // ---- 0. Build this pass's own Hi-Z ----
    // SSRDepthHierarchyPass binds the SHARED compute root signature, so it must
    // run BEFORE our own root signature is set (the same ordering RTReflectionPass
    // documents). Building it here rather than borrowing RTReflectionPass's makes
    // SSGI independent of the reflection settings.
    {
        StageTimer t(gfx, cl, "SSGI.HiZ");
        // The pyramid is deliberately left readable after this pass so the
        // later RTReflectionPass can borrow it. Return it to UAV only at the
        // start of the next SSGI build.
        if (const RHI::Texture* hizTex = m_ws->hiZ->GetTexture())
            TransitionTex(gfx, *hizTex, m_ws->hiZState,
                          RHI::ResourceState::UNORDERED_ACCESS, cl);
        m_ws->hiZ->Execute(cl, gfx.GetTextureSRVGpuHandle(*hizSrc));
    }
    if (const RHI::Texture* hizTex = m_ws->hiZ->GetTexture())
        TransitionTex(gfx, *hizTex, m_ws->hiZState, RHI::ResourceState::SHADER_RESOURCE, cl);
    const uint64_t hierSrv  = m_ws->hiZ->GetSrvHandle();
    const uint32_t hierMips = m_ws->hiZ->GetMipCount();
    if (hierSrv == 0)  return bail("Hi-Z SRV 0 (EnsureTexture never sized it?)");
    if (hierMips == 0) return bail("Hi-Z mip count 0");

    if (m_ws->hiZOnly) return cl; // Keep RTR's same-frame pyramid without reading screen-colour history.
    if (!m_hist) return bail("no view bound (PassSet::BindView)");
    History& hist = *m_hist;

    if (!m_ws->result.IsValid())  return bail("result texture invalid");
    if (!m_ws->diffIn.IsValid())  return bail("diffIn texture invalid");
    if (!hist.depthHistory.IsValid()) return bail("depth history texture invalid");
    if (m_reservoirEnabled &&
        (!m_ws->reservoirTemporal.IsValid() || !hist.reservoirHistory[0].IsValid() ||
         !hist.reservoirHistory[1].IsValid()))
        return bail("reservoir textures invalid");
    if (!m_ws->nrd)               return bail("no NRD instance");
    if (!m_ws->nrd->IsReady())    return bail("NRD not ready");
    // A missing CB does not fail any D3D12 call - it just leaves b0 unbound and
    // every thread reads zeros. Refuse the dispatch so the failure is visible.
    if (!m_cb.IsValid())      return bail("constant buffer not created");

    // ---- Fill CB ----
    {
        using namespace DirectX;
        SSGIConstants cb{};
        const XMMATRIX vp    = XMLoadFloat4x4(&m_ws->fd.viewProj);
        const XMMATRIX prevVP = XMLoadFloat4x4(&m_ws->fd.prevViewProj);
        const XMMATRIX invVP = XMMatrixInverse(nullptr, vp);
        const XMMATRIX prevInvVP = XMMatrixInverse(nullptr, prevVP);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), XMMatrixTranspose(invVP));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),    XMMatrixTranspose(vp));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
                        XMMatrixTranspose(prevVP));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevInvViewProj),
                        XMMatrixTranspose(prevInvVP));
        cb.cameraPos[0] = m_ws->fd.cameraPos.x;
        cb.cameraPos[1] = m_ws->fd.cameraPos.y;
        cb.cameraPos[2] = m_ws->fd.cameraPos.z;
        cb.maxDistance  = maxDistance;
        cb.thickness    = thickness;
        // screenW/H stay FULL res: they drive the uv, the Hi-Z march and every
        // screen-space tolerance, all of which must keep counting in the
        // resolution the depth hierarchy actually has.
        cb.screenW = m_ws->w; cb.screenH = m_ws->h;
        cb.invScreenW = 1.0f / float(m_ws->w);
        cb.invScreenH = 1.0f / float(m_ws->h);
        cb.traceW = m_ws->tw; cb.traceH = m_ws->th;
        cb.invTraceW = 1.0f / float(m_ws->tw);
        cb.invTraceH = 1.0f / float(m_ws->th);
        cb.nearZ = m_ws->fd.nearZ; cb.farZ = m_ws->fd.farZ;
        cb.frameIndex = m_ws->fd.frameIndex;
        // Reservoir mode emits exactly one fresh ray at half resolution (0.25
        // full-resolution rpp); the multi-ray mode keeps its >= 2 clamp.
        cb.rayCount = useReservoirThisFrame ? 1u : std::max(rayCount, 2u);
        cb.hizMipCount       = hierMips;
        cb.maxTraversalIters = std::max(maxTraversalIters, 8u);
        // Radiance and depth history are one validity domain. After a cut,
        // resize or enable edge, accepting the old scene-color pyramid before
        // current depth has repopulated would create one frame of stale GI.
        cb.hasPrevHdr        = (m_ws->fd.prevHdrSrv != 0 &&
                                hist.depthValid && !hist.reset) ? 1u : 0u;
        if(hist.depthValid && !hist.reset && !Sys::CommandLine::Get().Has("-ssgi_no_jitter_reprojection"))
        {
            cb.historyJitterDelta[0]=hist.jitterX-m_ws->fd.jitterX;
            cb.historyJitterDelta[1]=hist.jitterY-m_ws->fd.jitterY;
        }
        if(Sys::CommandLine::Get().Has("-restir_reprojection_log"))
            LOG_INFO("[RESTIR_REPROJECT] pass=SSGI frame=%u currentX=%.6f currentY=%.6f previousX=%.6f previousY=%.6f deltaX=%.6f deltaY=%.6f reset=%u",
                m_ws->fd.frameIndex,m_ws->fd.jitterX,m_ws->fd.jitterY,hist.jitterX,hist.jitterY,
                cb.historyJitterDelta[0],cb.historyJitterDelta[1],hist.reset?1u:0u);
        cb.prevHdrMaxMip     = m_ws->fd.prevHdrMaxMip;
        cb.ddgiVolumeCount   = m_ws->fd.ddgiVolumeCount;
        cb.ddgiEnabled       = m_ws->fd.ddgiEnabled;
        cb.normalBias        = normalBias;
        // MUST equal NrdDenoiser::Params::diffHitDistScaleA below. Both read
        // HitDistParamA() so they cannot drift.
        cb.hitDistParamA     = HitDistParamA();
        cb.debugMode         = debugMode;
        // hist.reset covers enable, resize and camera cuts - the three cases where
        // last frame's coverage describes a different image.
        cb.covHistoryValid   = (hist.reset || !hist.covHistory[0].IsValid()) ? 0u : 1u;
        cb.reservoirEnabled = useReservoirThisFrame ? 1u : 0u;
        cb.reservoirHistoryValid = (useReservoirThisFrame &&
                                    hist.reservoirValid &&
                                    !hist.reset) ? 1u : 0u;
        cb.reservoirSpatialCandidates = std::min(reservoirSpatialCandidates, 8u);
        cb.reservoirMaxM = std::clamp(reservoirMaxM, 1u, 63u);
        cb.reservoirMaxAge = std::clamp(reservoirMaxAge, 1u, 63u);
        cb.reservoirValidationSteps = std::clamp(reservoirValidationSteps, 4u, 6u);
        cb.reservoirNormalThreshold = std::clamp(reservoirNormalThreshold, 0.0f, 1.0f);
        cb.reservoirDepthThreshold = std::clamp(reservoirDepthThreshold, 0.001f, 0.10f);
        auto* slot = m_cb.Current(gfx);
        if (!slot) return bail("constant buffer slot null this frame");
        *slot = cb;
    }

    // Trace + NRD run at half res; only the resolve is full res.
    const uint32_t tgx = (m_ws->tw + 7u) / 8u;
    const uint32_t tgy = (m_ws->th + 7u) / 8u;
    const uint32_t gx  = (m_ws->w  + 7u) / 8u;
    const uint32_t gy  = (m_ws->h  + 7u) / 8u;

    // Binds @p pso and with it our layout - after another root signature (NRD's,
    // or none yet) that invalidates every root argument - then the arguments
    // every dispatch shares.
    auto bindCommon = [&](const RHI::PipelineState& pso)
    {
        gfx.BindComputePipelineState(pso, cl);
        gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
        gfx.SetComputeDescriptorTable(kSRV_Depth,    depthSrv,    cl);
        gfx.SetComputeDescriptorTable(kSRV_Normal,   normalSrv,   cl);
        gfx.SetComputeDescriptorTable(kSRV_Velocity, velocitySrv, cl);
        gfx.SetComputeDescriptorTable(kSRV_DepthHier, hierSrv, cl);
        // Type-correct fallbacks keep every private-root table bound even when
        // hasPrevHdr is zero; the shader never reads them in that state.
        gfx.SetComputeDescriptorTable(kSRV_PrevHDR,
                                      m_ws->fd.prevHdrSrv ? m_ws->fd.prevHdrSrv : normalSrv, cl);
        gfx.SetComputeDescriptorTable(kSRV_PrevDepth,
                                      hist.depthHistory.IsValid()
                                          ? gfx.GetTextureSRVGpuHandle(hist.depthHistory)
                                          : depthSrv, cl);
        if (m_ws->fd.ddgiVolumeBufSrv)
            gfx.SetComputeDescriptorTable(kSRV_DDGIVolumes, m_ws->fd.ddgiVolumeBufSrv, cl);
        if (m_ws->fd.ddgiIrradianceTable)
            gfx.SetComputeDescriptorTable(kSRV_DDGIIrr,   m_ws->fd.ddgiIrradianceTable, cl);
        if (m_ws->fd.ddgiDepthTable)
            gfx.SetComputeDescriptorTable(kSRV_DDGIDepth, m_ws->fd.ddgiDepthTable, cl);
        if (m_ws->fd.ddgiProbeDataTable)
            gfx.SetComputeDescriptorTable(kSRV_DDGIProbe, m_ws->fd.ddgiProbeDataTable, cl);
    };

    auto transitionReservoir = [&](ReservoirSet& r, RHI::ResourceState to)
    {
        TransitionTex(gfx, r.direction, r.directionState, to, cl);
        TransitionTex(gfx, r.radiance,  r.radianceState,  to, cl);
        TransitionTex(gfx, r.aux,       r.auxState,       to, cl);
        TransitionTex(gfx, r.meta,      r.metaState,      to, cl);
    };
    auto bindReservoirHistorySRV = [&](const ReservoirSet& r)
    {
        gfx.SetComputeDescriptorTable(kSRV_ReservoirHistoryDirection,
                                      gfx.GetTextureSRVGpuHandle(r.direction), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirHistoryRadiance,
                                      gfx.GetTextureSRVGpuHandle(r.radiance), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirHistoryAux,
                                      gfx.GetTextureSRVGpuHandle(r.aux), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirHistoryMeta,
                                      gfx.GetTextureSRVGpuHandle(r.meta), cl);
    };
    auto bindReservoirTemporalUAV = [&](const ReservoirSet& r)
    {
        gfx.SetComputeDescriptorTable(kUAV_ReservoirTemporalDirection,
                                      gfx.GetTextureUAVGpuHandle(r.direction), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirTemporalRadiance,
                                      gfx.GetTextureUAVGpuHandle(r.radiance), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirTemporalAux,
                                      gfx.GetTextureUAVGpuHandle(r.aux), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirTemporalMeta,
                                      gfx.GetTextureUAVGpuHandle(r.meta), cl);
    };
    auto bindReservoirTemporalSRV = [&](const ReservoirSet& r)
    {
        gfx.SetComputeDescriptorTable(kSRV_ReservoirTemporalDirection,
                                      gfx.GetTextureSRVGpuHandle(r.direction), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirTemporalRadiance,
                                      gfx.GetTextureSRVGpuHandle(r.radiance), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirTemporalAux,
                                      gfx.GetTextureSRVGpuHandle(r.aux), cl);
        gfx.SetComputeDescriptorTable(kSRV_ReservoirTemporalMeta,
                                      gfx.GetTextureSRVGpuHandle(r.meta), cl);
    };
    auto bindReservoirHistoryUAV = [&](const ReservoirSet& r)
    {
        gfx.SetComputeDescriptorTable(kUAV_ReservoirHistoryDirection,
                                      gfx.GetTextureUAVGpuHandle(r.direction), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirHistoryRadiance,
                                      gfx.GetTextureUAVGpuHandle(r.radiance), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirHistoryAux,
                                      gfx.GetTextureUAVGpuHandle(r.aux), cl);
        gfx.SetComputeDescriptorTable(kUAV_ReservoirHistoryMeta,
                                      gfx.GetTextureUAVGpuHandle(r.meta), cl);
    };

    // ---- 1. Trace + pack ----
    TransitionTex(gfx, m_ws->raw,         m_ws->rawState,         RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_ws->normalRough, m_ws->normalRoughState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_ws->viewZ,       m_ws->viewZState,       RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_ws->mv,          m_ws->mvState,          RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, m_ws->diffIn,      m_ws->diffInState,      RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, hist.depthHistory, hist.depthHistoryState,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);

    const uint32_t reservoirRead = 1u - hist.reservoirWrite;
    if (useReservoirThisFrame)
    {
        transitionReservoir(hist.reservoirHistory[reservoirRead],
                            RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        transitionReservoir(m_ws->reservoirTemporal,
                            RHI::ResourceState::UNORDERED_ACCESS);
    }

    bindCommon(m_tracePSO);
    gfx.SetComputeDescriptorTable(kUAV_Raw,         gfx.GetTextureUAVGpuHandle(m_ws->raw), cl);
    gfx.SetComputeDescriptorTable(kUAV_NormalRough, gfx.GetTextureUAVGpuHandle(m_ws->normalRough), cl);
    gfx.SetComputeDescriptorTable(kUAV_ViewZ,       gfx.GetTextureUAVGpuHandle(m_ws->viewZ), cl);
    gfx.SetComputeDescriptorTable(kUAV_MV,          gfx.GetTextureUAVGpuHandle(m_ws->mv), cl);
    gfx.SetComputeDescriptorTable(kUAV_DiffIn,      gfx.GetTextureUAVGpuHandle(m_ws->diffIn), cl);
    if (useReservoirThisFrame)
    {
        bindReservoirHistorySRV(hist.reservoirHistory[reservoirRead]);
        bindReservoirTemporalUAV(m_ws->reservoirTemporal);
    }
    {
        StageTimer t(gfx, cl, useReservoirThisFrame
                                  ? "SSGI.CandidateTemporal" : "SSGI.Trace");
        gfx.DispatchCompute(tgx, tgy, 1, cl);
    }

    if (useReservoirThisFrame)
    {
        // Trace produced the current temporal scratch. Spatial reads it and
        // writes the opposite final-history set; the transition is the required
        // UAV ordering point between the two dispatches.
        transitionReservoir(m_ws->reservoirTemporal,
                            RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        transitionReservoir(hist.reservoirHistory[hist.reservoirWrite],
                            RHI::ResourceState::UNORDERED_ACCESS);
        // The same layout: setting it again keeps the root arguments bound
        // (D3D12 invalidates them only when the root signature changes).
        gfx.BindComputePipelineState(m_reservoirSpatialPSO, cl);
        bindReservoirTemporalSRV(m_ws->reservoirTemporal);
        bindReservoirHistoryUAV(hist.reservoirHistory[hist.reservoirWrite]);
        {
            StageTimer t(gfx, cl, "SSGI.ReservoirSpatial");
            gfx.DispatchCompute(tgx, tgy, 1, cl);
        }
    }

    // ---- 2. NRD REBLUR_DIFFUSE ----
    TransitionTex(gfx, m_ws->normalRough, m_ws->normalRoughState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_ws->viewZ,       m_ws->viewZState,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_ws->diffIn,      m_ws->diffInState,      RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_ws->diffOut,     m_ws->diffOutState,     RHI::ResourceState::UNORDERED_ACCESS, cl);
    // mv stays in UNORDERED_ACCESS: REBLUR_DIFFUSE's temporal-stabilization
    // pass WRITES it. NRD transitions from the state we declare (the
    // UserTexture::state contract) and restores it before returning.

    NrdDenoiser::Params np{};
    std::memcpy(np.viewToClip,  &m_ws->fd.projMatrixNoJitter, sizeof(np.viewToClip));
    std::memcpy(np.worldToView, &m_ws->fd.viewMatrix,         sizeof(np.worldToView));
    np.jitter[0] = m_ws->fd.jitterX; np.jitter[1] = m_ws->fd.jitterY;
    np.frameIndex     = m_ws->fd.frameIndex;
    np.resetHistory   = hist.reset;
    np.denoisingRange = m_ws->fd.farZ * 0.99f;
    np.width = m_ws->tw; np.height = m_ws->th;
    // Jitter is in pixels of the resolution NRD works at - the half-res grid, where
    // the same sub-pixel offset is half as many pixels. The full-res value would
    // mis-align its un-jittering by half a texel every frame.
    np.jitter[0] *= 0.5f; np.jitter[1] *= 0.5f;
    // MIRROR of SSGIConstants::hitDistParamA. Both call HitDistParamA().
    np.diffHitDistScaleA = HitDistParamA();

    auto bind = [&](nrd::ResourceType type, const RHI::Texture& t, RHI::ResourceState st) {
        NrdDenoiser::Binding b;
        b.type = type; b.tex.texture = &t; b.tex.state = st;
        return b;
    };
    const NrdDenoiser::Binding bindings[] = {
        bind(nrd::ResourceType::IN_MV,               m_ws->mv,          RHI::ResourceState::UNORDERED_ACCESS),
        bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS, m_ws->normalRough, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_VIEWZ,            m_ws->viewZ,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,  m_ws->diffIn,  RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, m_ws->diffOut, RHI::ResourceState::UNORDERED_ACCESS),
    };
    {
        StageTimer t(gfx, cl, "SSGI.REBLUR");
        m_ws->nrd->Denoise(gfx, cl, np, bindings, 5);
    }

    // ---- 3. NRD trashed the heaps and the root signature - restore ----
    gfx.BindDescriptorHeaps(cl);
    bindCommon(m_resolvePSO);

    // ---- 4. Resolve ----
    TransitionTex(gfx, m_ws->diffOut, m_ws->diffOutState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, m_ws->raw,     m_ws->rawState,     RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    // With the result imported the graph transitions it (to UAV in this pass's
    // Phase A, back for each reader); this transition and the one after the
    // dispatch run only without the import (-rgimportoff).
    if (!m_resultH.IsValid())
        TransitionTex(gfx, m_ws->result, m_ws->resultState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, hist.depthHistory, hist.depthHistoryState,
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    // Ping-pong: read last frame's accumulated coverage, write this frame's.
    const uint32_t covRead = 1u - hist.covWrite;
    TransitionTex(gfx, hist.covHistory[covRead],  hist.covHistoryState[covRead],
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, hist.covHistory[hist.covWrite], hist.covHistoryState[hist.covWrite],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    gfx.SetComputeDescriptorTable(kSRV_DiffDenoised, gfx.GetTextureSRVGpuHandle(m_ws->diffOut), cl);
    gfx.SetComputeDescriptorTable(kSRV_RawCoverage,  gfx.GetTextureSRVGpuHandle(m_ws->raw), cl);
    gfx.SetComputeDescriptorTable(kUAV_Result,       gfx.GetTextureUAVGpuHandle(m_ws->result), cl);
    gfx.SetComputeDescriptorTable(kSRV_CovHistory,
                                  gfx.GetTextureSRVGpuHandle(hist.covHistory[covRead]), cl);
    gfx.SetComputeDescriptorTable(kUAV_CovHistory,
                                  gfx.GetTextureUAVGpuHandle(hist.covHistory[hist.covWrite]), cl);
    gfx.SetComputeDescriptorTable(kUAV_DepthHistory,
                                  gfx.GetTextureUAVGpuHandle(hist.depthHistory), cl);
    {
        StageTimer t(gfx, cl, "SSGI.Resolve");
        gfx.DispatchCompute(gx, gy, 1, cl);
    }
    m_ws->lastDispatchedPixels = m_ws->w * m_ws->h;   // proof Execute reached the end

    // Leave the result readable by the LIGHTING PIXEL SHADER: it is folded through
    // LightCB.ssgiResultIdx, a PS read, so the compute-only state is not enough.
    if (!m_resultH.IsValid())
        TransitionTex(gfx, m_ws->result, m_ws->resultState, RHI::ResourceState::SHADER_RESOURCE, cl);
    // Put the MV texture back where the next frame's transitions expect it.
    TransitionTex(gfx, m_ws->mv, m_ws->mvState, RHI::ResourceState::SHADER_RESOURCE, cl);

    hist.covWrite = 1u - hist.covWrite;   // flip AFTER the dispatch that wrote it
    if (useReservoirThisFrame)
    {
        hist.reservoirWrite = 1u - hist.reservoirWrite;
        hist.reservoirValid = true;
    }
    hist.reset = false;
    hist.depthValid = true;
    hist.jitterX=m_ws->fd.jitterX; hist.jitterY=m_ws->fd.jitterY;
    return cl;
}
