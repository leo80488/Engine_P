#include "RenderGraph/RenderPass/RTDIPass.h"
#include "RenderGraph/RenderPass/RTDIEngineCore.h"
#include "RenderGraph/RenderPass/RTDIRtxdiCore.h"
#include "Settings/PassSettings.h"     // pass.rtdi.*
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ReSTIRTransientTextures.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/WordHash.h"
#include <chrono>
#include <future>
#include <iterator>
#include <cstring>

namespace RTDI
{
const RegionNames kMainRegions{"RTDI.Clear","RTDI.Classify","RTDI.Prepare","RTDI.InitialShade","RTDI.Temporal",
    "RTDI.Spatial","RTDI.Spatial.1","RTDI.Shade","RTDI.Presample","RTDI.ReGIR","RTDI.Convert","RTDI.RELAX",
    "RTDI.REBLUR","RTDI.Resolve","RTDI.Boiling"};
const RegionNames kShadowRegions{"RTDI.Shadow.Clear","RTDI.Shadow.Classify","RTDI.Shadow.Prepare",
    "RTDI.Shadow.InitialShade","RTDI.Shadow.Temporal","RTDI.Shadow.Spatial","RTDI.Shadow.Spatial.1",
    "RTDI.Shadow.Shade","RTDI.Shadow.Presample","RTDI.Shadow.ReGIR","RTDI.Shadow.Convert","RTDI.Shadow.RELAX",
    "RTDI.Shadow.REBLUR","RTDI.Shadow.Resolve","RTDI.Shadow.Boiling"};
} // namespace RTDI

RTDIPass::RTDIPass(RG::RenderGraph& graph, RG::RGTextureHandle albedo, RG::RGTextureHandle normal,
    RG::RGTextureHandle surface, RG::RGTextureHandle extra, RG::RGTextureHandle depth,
    RG::RGTextureHandle velocity, RG::RGTextureHandle result, bool shadow)
    : m_shadow(shadow),m_regions(shadow?&RTDI::kShadowRegions:&RTDI::kMainRegions),
      m_graph(graph),m_albedo(albedo),m_normal(normal),m_surface(surface),m_extra(extra),
      m_depth(depth),m_velocity(velocity),m_result(result) {}

RTDIPass::~RTDIPass() = default;

void RTDIPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_albedo); b.ReadSRV(m_normal); b.ReadSRV(m_surface);
    b.ReadSRV(m_extra); b.ReadSRV(m_depth); b.ReadSRV(m_velocity); b.WriteUAV(m_result);
}

void RTDIPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this, GetName());
    if (view.Slot() == 0)
        m_ws = &m_main;
    else
    {
        ViewWork& work = view.Data<ViewWork>(this);
        work.owner = this;
        m_ws = &work.ws;
    }
}

void RTDIPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for (auto& tex:ws.tex) { if(tex.IsValid()) gfx.DestroyTexture(tex); tex={}; }
    for (auto* b:{&ws.list,&ws.counter,&ws.args}) { if(b->IsValid()) gfx.DestroyBuffer(*b); *b={}; }
    if (ws.core) ws.core->Release(gfx);
    if (owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd),RHI::kFramesInFlight+1u);
    ws=WorkingSet{};
}

void RTDIPass::Init(IGraphicsDevice& gfx)
{
    ENGINE_CHECK(m_hist != nullptr, "%s: no view bound before Init (PassSet::SetView before its registration)",
                 GetName());
    m_gfx=&gfx;
    if (!gfx.GetCapabilities().inlineRaytracing) return;
    const auto& cmd=Sys::CommandLine::Get();
    // The shader's flag word: the shell parses it; the core owns bit 8 (reuse) and adds its
    // reuse-chain bits per frame (RTDI::Core::FillConstants). Bits 1 and 4 (no NRD, full
    // resolution) come from the settings (ApplySettingFlags); -di_no_nrd / -di_fullres pin
    // restirDiDenoiser / restirDiFullRes.
    m_flags=(cmd.Has("-di_no_visibility")?2u:0u)
        |(cmd.Has("-di_no_spatial")?16u:0u)|(cmd.Has("-di_reject_all")?32u:0u)|(cmd.Has("-di_no_nrd_antifirefly")?64u:0u)|(cmd.Has("-di_history_counts")?128u:0u)|(cmd.Has("-di_nrd_target")?256u:0u);
    if(cmd.Has("-di_confidence") || cmd.Has("-di_confidence_zero") || cmd.Has("-di_confidence_one")) m_flags|=512u;
    if(cmd.Has("-di_visibility_confidence")) m_flags|=512u|(1u<<24);
    // -di_history_counts reads REBLUR's returnHistoryLengthInsteadOfOcclusion,
    // a REBLUR-only diagnostic, so it keeps that denoiser. Otherwise the
    // denoiser setting decides (ApplySettingFlags).
    m_reblurPinned=cmd.Has("-di_history_counts");
    m_relax=!m_reblurPinned;
    // RELAX runs as RELAX_DIFFUSE + RELAX_SPECULAR (NrdDenoiser::Kind::RelaxDiffuseSpecularSplit)
    // so anti-firefly can be specular-only; -di_nrd_combined selects the single
    // RELAX_DIFFUSE_SPECULAR.
    m_nrdSplit=!cmd.Has("-di_nrd_combined");
    if(cmd.Has("-di_confidence_zero")) m_flags|=1024u;
    else if(cmd.Has("-di_confidence_one")) m_flags|=2048u;
    if(cmd.Has("-restir_no_jitter_reprojection")) m_flags|=16384u;
    m_mirrorRoughness=std::clamp(::Settings::Pass::rtdiRawMirrorRoughness(),0.f,1.f);
    if(cmd.Has("-di_spatial_integrate"))
    {
        m_flags|=4096u;
        if(std::clamp(cmd.GetInt("-di_spatial_passes",1),0,2)!=1)
        { LOG_ERROR("RTDIPass: spatial integration diagnostic requires one spatial pass"); return; }
    }
    const uint32_t debugMode=static_cast<uint32_t>(std::clamp(cmd.GetInt("-di_debug",0),0,3));
    m_flags|=debugMode<<16; // 1 calibration, 2 magenta, 3 isolated DI signal
    // engine.json restirDiFullRes sets flag 4 (ApplySettingFlags; -di_fullres pins it); the
    // calibration arm forces it.
    m_fullResForced=debugMode==1;
    // A/B only: under ray reconstruction keep the configured resolution
    // (ApplySettingFlags traces full resolution there).
    m_rrHalfRes=cmd.Has("-di_rr_halfres");
    // Factored composition (dev): App.cpp refuses a malformed value. The composited pass's only:
    // the compare mode's shadow runs whole cores.
    if (!m_shadow)
    {
        ReSTIR::Backend half;
        m_initialHalf=ReSTIR::ParseHalf(cmd.Get("-di_backend_initial",""),half) ? half : ReSTIR::Backend::Count;
        m_reuseHalf=ReSTIR::ParseHalf(cmd.Get("-di_backend_reuse",""),half) ? half : ReSTIR::Backend::Count;
    }
    if(cmd.Has("-di_boundary_probe") || cmd.Has("-di_boundary_wrong_remod")) m_flags|=1u<<18;
    if(cmd.Has("-di_boundary_wrong_remod")) m_flags|=1u<<19;
    if(cmd.Has("-di_exact_diffuse_demod")) m_flags|=1u<<20;
    if(cmd.Has("-di_chroma_demod")) m_flags|=1u<<21;
    // The centered tent Resolve is the default; -di_box_resolve selects the 3x3 box gather
    // (-di_centered_resolve stays accepted). A Resolve whose weights all but vanish falls back to
    // the clustered loop (-di_resolve_no_fallback normalizes them instead).
    const bool boxResolve=cmd.Has("-di_box_resolve");
    if(cmd.Has("-di_centered_resolve") || !boxResolve) m_flags|=1u<<23;
    // The 2x2 trace phase is fixed by default: rotating it every frame doubled the lamp-highlight
    // flicker on thin glossy geometry under DLSS (the y phase alternates, so 1-pixel highlight
    // lines were traced every other frame: 4858 -> 8748 flickering pixels on SchoolNight).
    // -di_rotate_phase rotates it; -di_fixed_phase stays accepted.
    const bool fixedPhase=cmd.Has("-di_fixed_phase");
    m_rotatePhase=cmd.Has("-di_rotate_phase") && !fixedPhase;
    // DI specular AA (RTDI.cs.hlsl DIApplySpecularAA): widen the receiver's
    // GGX lobe by the normal variance across its 2x2 block, x the extra
    // footprint the half-resolution texel and the AA jitter add over the
    // GBuffer's own one-pixel specular AA. pass.rtdi.specAA (0 = off).
    const float specAA=::Settings::Pass::rtdiSpecAA();
    m_specularAA=std::isfinite(specAA)?std::clamp(specAA,0.f,63.75f):kDefaultSpecularAA;
    m_flags2=(cmd.Has("-di_resolve_no_fallback")?0u:1u)
        // Geometric-normal ray origin + horizon; -di_shading_origin selects the P+1mm*N origin.
        |(cmd.Has("-di_shading_origin")?0u:2u)
        // A/B only: the stricter confidence rules (RTDI.cs.hlsl flags2 bit 2).
        |(cmd.Has("-di_confidence_require_valid")?4u:0u)
        |(static_cast<uint32_t>(std::lround(m_specularAA*4.f))<<8);
    if (!m_shadow)
    {
        const bool afOff=(m_flags&64u)!=0, afOptIn=cmd.Has("-di_nrd_antifirefly"), split=m_relax && m_nrdSplit;
        LOG_INFO("[RTDI_SPECAA] scale=%.2f rotatePhase=%u nrdSplit=%u antiFireflyDiffuse=%u antiFireflySpecular=%u",
            m_specularAA,m_rotatePhase?1u:0u,split?1u:0u,
            (split ? (afOptIn && !afOff) : (!afOff || afOptIn))?1u:0u,(!afOff)?1u:0u);
    }
    if(cmd.Has("-di_nrd_fullres_radii")) m_flags|=1u<<22;
    m_translateLights=!cmd.Has("-di_no_light_translation");
    if(debugMode==1) m_flags=(m_flags|1u|2u|4u)&~8u; // full-res, no NRD/visibility/reuse
    m_noNrdForced=(m_flags&1u)!=0;
    // raytracing.diRayTMax arrives with SetSettings (-di_ray_tmax pins it); a reload keeps the
    // distance the core was last given.
    if(!m_haveCoreSettings) m_rayTMax=150.f;
    const float nrdScale=::Settings::Pass::rtdiNrdHitDistScale();
    m_nrdHitDistScale=std::isfinite(nrdScale)?std::clamp(nrdScale,.25f,4.f):1.f;
    // The core: created here with the shell's flag word; a backend switch replaces it at the next
    // EnsureResources.
    ApplySettingFlags();
    m_main.core=std::make_unique<RTDI::EngineCore>(m_flags);
    m_coreComposition={};
    m_main.core->ApplyBudgetStage(m_budgetStage);
    if(m_haveCoreSettings) { bool reset=false; m_main.core->ApplySettings(m_coreSettings,m_rayTMax,false,reset); }
    if(m_main.core->Reuse()) m_flags|=8u;
    m_backendDirty=true;
    ++m_generation;   // a secondary view's core records with this one's pipelines
    if (!ReSTIR::CreateShellLayout(gfx,m_layout,GetName()))
    { LOG_ERROR("RTDIPass: pipeline layout failed"); return; }
    // The pipelines are built on first use (BuildPipelines, from EnsureResources);
    // the cores' indirect dispatches use the device's dispatch signature.
    m_pipelinesFailed=false;
    if (!m_cb.Create(gfx,"RTDI.Constants")) LOG_ERROR("RTDIPass: constants failed");
}

bool RTDIPass::BuildPipelines()
{
    if (m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines()) return true;
    if (m_pipelinesFailed || !m_layout.IsValid() || !m_gfx || !m_main.core) return false;
    IGraphicsDevice& gfx=*m_gfx;
    const auto start=std::chrono::steady_clock::now();
    // The core's kernels on a thread of their own while the shell's are built
    // here, each set's pipelines concurrently (ComputePipelineBatch, as
    // RTGIPass). Each half writes only its own pipelines.
    std::future<bool> core=std::async(std::launch::async,[&]{ return m_main.core->BuildPipelines(gfx,m_layout); });
    static constexpr const char* kEntries[]={"Clear","Classify","Prepare","Resolve"};
    static_assert(std::size(kEntries)==std::tuple_size_v<decltype(m_pso)>);
    const bool shellOk=m_pso.back().IsValid() || ComputePipelineBatch::BuildEntries(gfx,m_layout,"shaders/RTDI.cs.hlsl",
                                                                                   kEntries,m_pso.size(),m_pso.data(),"RTDIPass");
    const bool coreOk=core.get();
    if (!shellOk || !coreOk) { m_pipelinesFailed=true; return false; }
    LOG_INFO("RTDIPass: pipelines built on first use (%.0f ms)",
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    return true;
}

void RTDIPass::SetBackend(ReSTIR::Backend requested)
{
    if (!ReSTIR::IsLegal(ReSTIR::Stage::DI,requested)) requested=ReSTIR::Backend::Engine;
    if (requested!=m_requested) { m_requested=requested; m_backendDirty=true; }
}

void RTDIPass::ApplyBackend()
{
    if (!m_backendDirty) return;
    m_backendDirty=false;
    auto requested=ReSTIR::Decompose(m_requested);
    if (m_initialHalf!=ReSTIR::Backend::Count) requested.initial=m_initialHalf;
    if (m_reuseHalf!=ReSTIR::Backend::Count) requested.reuse=m_reuseHalf;
    m_status.requested=m_requested;
    m_status.requestedName=ReSTIR::CompositionName(requested);
    // A composition with an RTXDI half runs on the RTXDI core when this build and device can;
    // otherwise the engine core runs instead, loudly, with RtxdiInertReason's reason.
    ReSTIR::DIComposition effective{};
    const char* reason="selected";
    if (requested!=effective)
    {
        const bool native16=m_gfx && m_gfx->GetCapabilities().native16BitShaderOps;
        if (const char* inert=ReSTIR::RtxdiInertReason(ReSTIR::Stage::DI,native16)) reason=inert;
        else effective=requested;
    }
    if (!m_main.core || effective!=m_coreComposition)
    {
        // The switch: the old core's working set and pipelines go, the new one builds on first use
        // (EnsureResources) and starts with an empty history; the shell's set (NRD, the transient
        // textures, the counters, the result) stays. At the frame boundary: the GPU may still hold
        // the old core's pipelines.
        std::unique_ptr<RTDI::Core> next;
        if (effective!=ReSTIR::DIComposition{}) next=RTDI::MakeRtxdiCore(m_flags,effective);
        if (!next && effective!=ReSTIR::DIComposition{}) { effective={}; reason="rtxdi sdk not in this build"; }
        if (!next) next=std::make_unique<RTDI::EngineCore>(m_flags);
        if (m_main.core && m_gfx)
        {
            m_gfx->WaitIdleAndReleaseDeferred();
            const uint64_t bytes=m_main.core->IsAllocated()?m_main.core->ResidentBytes():0;
            if (m_main.core->IsAllocated()) m_main.core->Release(*m_gfx);
            m_main.core->ReleasePipelines(*m_gfx);
            LOG_INFO("[RESTIR_SWITCH] pass=%s from=%s to=%s releasedBytes=%llu",GetName(),
                ReSTIR::CompositionName(m_coreComposition),ReSTIR::CompositionName(effective),
                static_cast<unsigned long long>(bytes));
        }
        m_main.core=std::move(next);
        m_coreComposition=effective;
        ++m_generation;   // the views' cores are made again with it
        m_main.core->ApplyBudgetStage(m_budgetStage);
        if (m_haveCoreSettings) { bool reset=false; m_main.core->ApplySettings(m_coreSettings,m_rayTMax,false,reset); }
        m_flags=m_main.core->Reuse()?(m_flags|8u):(m_flags&~8u);
        m_hist->reset=true; m_hist->nrdReset=true;
    }
    m_status.effective=effective==ReSTIR::DIComposition{} ? ReSTIR::Backend::Engine
        : (effective.initial==ReSTIR::Backend::Rtxdi && effective.reuse==ReSTIR::Backend::Engine) ? ReSTIR::Backend::Hybrid
        : ReSTIR::Backend::Rtxdi;
    m_status.effectiveName=ReSTIR::CompositionName(effective);
    m_status.reason=reason;
    m_status.ignored=m_main.core ? m_main.core->IgnoredFlags() : "";
    m_logBackend=true; // once the working set exists (residentBytes)
}

void RTDIPass::ApplySettingFlags()
{
    // -di_debug=1 already forced (1|2|4)&~8 in Init. Under ray reconstruction with the Auto
    // denoiser NRD is skipped and the trace runs at full resolution: the half-resolution Resolve's
    // 3x3 tent spreads one raw sample over ~5x5 render pixels, spatially correlated noise RR
    // assumes it never gets ("independent samples") and keeps as detail - blotches that boil
    // every frame (11,576 large jumps a frame on SchoolNight at DLSS Quality against 1,022 at full
    // resolution). On the NRD path RELAX denoises at trace resolution before that upsample, so
    // half resolution stays the default there. -di_rr_halfres: A/B. RELAX / REBLUR keep NRD
    // before RR (it denoises before the upsample, so the configured resolution stands); None is
    // the raw signal with or without RR.
    using ReSTIR::Denoiser;
    const bool noNrd=m_noNrdForced || m_denoiserSetting==Denoiser::None
        || (m_rrRaw && m_denoiserSetting==Denoiser::Auto);
    if (m_fullResForced || m_fullResSetting || (m_rrRaw && noNrd && !m_rrHalfRes)) m_flags|=4u;
    else m_flags&=~4u;
    if (noNrd) m_flags|=1u;
    else m_flags&=~1u;
    m_relax=!m_reblurPinned && m_denoiserSetting!=Denoiser::Reblur;
}

uint32_t RTDIPass::ViewFlags() const
{
    // ApplySettingFlags without ray reconstruction's terms.
    uint32_t flags=m_flags&~(1u|4u);
    if (m_fullResForced || m_fullResSetting) flags|=4u;
    if (m_noNrdForced || m_denoiserSetting==ReSTIR::Denoiser::None) flags|=1u;
    return flags;
}

std::unique_ptr<RTDI::Core> RTDIPass::MakeCore(uint32_t shellFlags,const ReSTIR::DIComposition& composition)
{
    std::unique_ptr<RTDI::Core> core;
    if (composition!=ReSTIR::DIComposition{}) core=RTDI::MakeRtxdiCore(shellFlags,composition);
    if (!core) core=std::make_unique<RTDI::EngineCore>(shellFlags);
    return core;
}

void RTDIPass::SetRayReconstruction(bool on)
{
    if (on==m_rrRaw) return;
    m_rrRaw=on;
    const uint32_t before=m_flags;
    ApplySettingFlags();
    if ((before^m_flags)&(1u|4u)) { m_main.resourcesReady=false; m_hist->reset=true; m_hist->nrdReset=true; }
    LOG_INFO("[RESTIR_RR] stage=%s rayReconstruction=%u nrd=%u fullResolution=%u",m_shadow?"DI.Shadow":"DI",
        on?1u:0u,(m_flags&1u)?0u:1u,(m_flags&4u)?1u:0u);
}

void RTDIPass::LogBackend() const
{
    LOG_INFO("[RESTIR_BACKEND] stage=%s requested=%s effective=%s reason=%s pipelineMs=%.1f residentBytes=%llu ignored=%s",
        m_shadow?"DI.Shadow":"DI",m_status.requestedName,m_status.effectiveName,m_status.reason,
        m_status.pipelineMs,static_cast<unsigned long long>(m_status.residentBytes),
        m_status.ignored.empty()?"-":m_status.ignored.c_str());
}

void RTDIPass::EnsureResources(bool wanted,bool retain)
{
    m_wanted=wanted;
    if (!m_gfx) return;
    if (!wanted)
    {
        // A pass that is not wanted gives its core's reservoirs back; the next wanted frame
        // re-Allocates them and starts a new history (OnCompile).
        if (!retain && m_main.core && m_main.core->IsAllocated())
        {
            const uint64_t bytes=m_main.core->ResidentBytes();
            m_main.core->Release(*m_gfx);
            m_status.residentBytes=0;
            LOG_INFO("[RESTIR_RELEASE] pass=%s backend=%s coreBytes=%llu",GetName(),m_status.effectiveName,static_cast<unsigned long long>(bytes));
        }
        return;
    }
    ApplyBackend();
    BuildPipelines();
    if (m_main.core) m_status.pipelineMs=m_main.core->PipelineMs();
    // RestirDIResult is an ON-DEMAND graph texture (Renderer::Compile): the
    // first frame this pass is wanted with a pipeline to write it is what backs
    // it, and it stays backed (RenderGraph::EnsurePhysicalTexture).
    if (m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines() && m_cb.IsValid()) m_graph.EnsurePhysicalTexture(m_result,*m_gfx);
    OnCompile(*m_gfx);
    if (m_logBackend && (m_main.resourcesReady || m_pipelinesFailed))
    {
        if (m_pipelinesFailed) m_status.reason="pipeline build failed";
        LogBackend();
        m_logBackend=false;
    }
}

void RTDIPass::SetSettings(const Settings& in)
{
    if (!m_main.core) return;
    const float d=std::clamp(in.distance,1.f,1000.f);   // raytracing.diRayTMax (refuses NaN)
    // Full resolution changes the trace size: the working set is rebuilt at
    // this service point, the history with it.
    m_fullResSetting=in.fullResolution;
    // A filter setting of the next denoise: no history reset.
    if (in.specularAntiFirefly!=m_specAntiFireflySetting)
        LOG_INFO("[RTDI_SETTINGS] specularAntiFirefly=%u",in.specularAntiFirefly?1u:0u);
    m_specAntiFireflySetting=in.specularAntiFirefly;
    const bool wasFull=(m_flags&4u)!=0;
    const uint32_t flagsBefore=m_flags; const bool relaxBefore=m_relax;
    m_denoiserSetting=in.denoiser;
    ApplySettingFlags();
    if (((m_flags&4u)!=0)!=wasFull)
    {
        m_main.resourcesReady=false; m_hist->reset=true;
        LOG_INFO("[RTDI_SETTINGS] fullResolution=%u",(m_flags&4u)?1u:0u);
    }
    // The denoiser picks NRD's kind and its pools at the working set's
    // allocation (OnCompile): a change rebuilds it, history included.
    if (((flagsBefore^m_flags)&1u) || relaxBefore!=m_relax)
    {
        m_main.resourcesReady=false; m_hist->reset=true; m_hist->nrdReset=true;
        if (!m_shadow)   // the compare shadow follows the same settings
            LOG_INFO("[RTDI_SETTINGS] denoiser=%s nrd=%s",ReSTIR::DenoiserName(m_denoiserSetting),
                (m_flags&1u)?"none":(m_relax?"relax":"reblur"));
    }
    RTDI::CoreSettings s;
    s.spatialPasses=in.spatialPasses; s.spatialRadius=in.spatialRadius; s.spatialK=in.spatialK;
    s.maxM=in.maxM; s.maxAge=in.maxAge; s.candidates=in.candidates; s.reuse=in.reuse;
    s.reusePinned=in.reusePinned; s.spatialPassesPinned=in.spatialPassesPinned;
    s.spatialRadiusPinned=in.spatialRadiusPinned; s.ageCapPinned=in.ageCapPinned;
    m_coreSettings=s; m_haveCoreSettings=true;
    bool historyReset=false;
    const bool realloc=m_main.core->ApplySettings(s,d,d!=m_rayTMax,historyReset);
    // The RTXDI core's ignored report names command-line settings too.
    m_status.ignored=m_main.core->IgnoredFlags();
    const bool wantReuse=m_main.core->Reuse();
    if (wantReuse!=((m_flags&8u)!=0))
    {
        // Reservoir and receiver buffers exist only with reuse; OnCompile
        // (re)builds them at the same service point as a spatial-pass change.
        m_flags=wantReuse?(m_flags|8u):(m_flags&~8u);
        m_main.resourcesReady=false;
    }
    if (realloc) m_main.resourcesReady=false;
    if (historyReset) m_hist->reset=true;
    m_rayTMax=d;
}

void RTDIPass::ReleaseWorkingSet(IGraphicsDevice& gfx,WorkingSet& ws)
{
    const bool main=&ws==&m_main;
    // NRD pools release immediately: the main view's resource changes run only at the safe
    // service/compile point and drain before dropping one; a secondary view's (its segment)
    // retire it.
    if (ws.nrd)
    {
        if (main) { gfx.WaitIdleAndReleaseDeferred(); ws.nrd.reset(); }
        else m_retiredNrd.emplace_back(std::move(ws.nrd),RHI::kFramesInFlight+1u);
    }
    for (auto& tex:ws.tex) { if(tex.IsValid()) gfx.DestroyTexture(tex); tex={}; }
    if (main)
        for (uint32_t i=0;i<RHI::kFramesInFlight;++i)
        {
            if(m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
            if(m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
            m_readback[i]={}; m_mapped[i]=nullptr; m_primed[i]=false;
        }
    for (auto* b:{&ws.list,&ws.counter,&ws.args}) { if(b->IsValid()) gfx.DestroyBuffer(*b); *b={}; }
    ws.phase=0;
    if (ws.core) ws.core->Release(gfx);
    ws.resourcesReady=false;
    ws.w=ws.h=0; // the next build reallocates even at an unchanged size
    if (main) m_status.residentBytes=0;
}

void RTDIPass::OnCompile(IGraphicsDevice& gfx)
{
    // The main view's (the graph's compile and the service point); a secondary view's is made in its
    // segment (EnsureViewResources).
    if (m_ws!=&m_main) return;
    BuildWorkingSet(gfx,gfx.GetRenderWidth(),gfx.GetRenderHeight());
}

void RTDIPass::EnsureViewResources(uint32_t w,uint32_t h,bool wanted)
{
    if (m_ws==&m_main || !m_gfx || !m_hist) return;
    WorkingSet& ws=*m_ws;
    const bool can=wanted && m_wanted && m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines()
        && m_cb.IsValid();
    // Another core on the main view (a backend switch, a reload) or another flag word or denoiser:
    // this view's set is made again.
    const uint32_t flags=ViewFlags();
    if (ws.core && (!can || ws.generation!=m_generation || ws.flags!=flags || ws.relax!=m_relax))
    {
        ReleaseWorkingSet(*m_gfx,ws);
        ws.core.reset();
    }
    if (!can) return;
    if (!ws.core)
    {
        ws.flags=flags; ws.relax=m_relax; ws.generation=m_generation; ws.rayTMax=0;
        ws.core=MakeCore(flags,m_coreComposition);
        ws.core->AdoptPipelines(*m_main.core);
    }
    // The main view's settings, as OnService gives its core every frame.
    ws.core->ApplyBudgetStage(m_budgetStage);
    if (m_haveCoreSettings)
    {
        bool reset=false;
        if (ws.core->ApplySettings(m_coreSettings,m_rayTMax,m_rayTMax!=ws.rayTMax,reset)) ws.resourcesReady=false;
        if (reset) m_hist->reset=true;
        ws.rayTMax=m_rayTMax;
    }
    BuildWorkingSet(*m_gfx,w,h);
}

void RTDIPass::BuildWorkingSet(IGraphicsDevice& gfx,uint32_t w,uint32_t h)
{
    WorkingSet& ws=*m_ws;
    const bool main=m_ws==&m_main;
    if (!m_wanted || !m_pso.back().IsValid() || !ws.core || !ws.core->HasPipelines() || !m_cb.IsValid()) return;
    if (!w || !h) return;
    const uint32_t flags=BoundFlags();
    if (w==ws.w && h==ws.h && ws.resourcesReady && (!ws.placedMask || !m_aliasArena || ws.placedEpoch==m_aliasArena->Epoch()))
    {
        // The shell's set is current; the core's may have been released
        // while the pass was not wanted (EnsureResources). Its history is new.
        if (!ws.core->IsAllocated())
        {
            if (!ws.core->Allocate(gfx,ws.tw,ws.th)) { ws.resourcesReady=false; return; }
            m_hist->reset=true;
            if (main) m_status.residentBytes=ws.core->ResidentBytes();
        }
        return;
    }
    ReleaseWorkingSet(gfx,ws);
    ws.w=w; ws.h=h; const uint32_t scale=(flags&4u)?1:2;
    ws.tw=(w+scale-1)/scale; ws.th=(h+scale-1)/scale;
    const auto& formats=ReSTIRTransient::diFormats;
    static_assert(ReSTIRTransient::diFormats.size()==TextureCount);
    const auto layout=ReSTIRTransient::Measure(gfx,ws.tw,ws.th,formats);
    // The main view's textures in the alias arena; a secondary view's are committed (the arena's
    // disjointness argument is the main view's pass order).
    const bool useArena=main && m_aliasArena && layout.bytes && !Sys::CommandLine::Get().Has("-restir_no_alias");
    // The first ReSTIR working set reserves the arena's worst case (a session that never enables ReSTIR
    // never pays for it).
    if(useArena) ReSTIRTransient::Reserve(gfx,*m_aliasArena);
    const bool placed=useArena && gfx.EnsureAliasArena(*m_aliasArena,layout.bytes);
    // Placed-resource creation itself may initialize metadata over live bytes.
    // This runs only at the serial resource-service boundary, never recording.
    if(placed) gfx.WaitIdleAndReleaseDeferred();
    ws.placedMask=0;
    for(size_t i=0;i<ws.tex.size();++i)
    {
        if(i==Confidence && !(flags&512u)) continue;
        auto td=ReSTIRTransient::Describe(ws.tw,ws.th,formats[i]);
        ws.states[i]=td.layout;
        if(placed && gfx.CreateTexturePlaced(td,*m_aliasArena,layout.offsets[i],ws.tex[i])) ws.placedMask|=1u<<i;
        else if(!gfx.CreateTexture(td,ws.tex[i])) { LOG_ERROR("RTDIPass: texture %zu failed",i); return; }
    }
    ws.placedEpoch=(placed && m_aliasArena)?m_aliasArena->Epoch():0;
    if (main) LOG_INFO("[RESTIR_ARENA] pass=RTDI width=%u height=%u placedMask=%u bytes=%llu epoch=%u",
        ws.tw,ws.th,ws.placedMask,static_cast<unsigned long long>(layout.bytes),ws.placedEpoch);
    RHI::GPUBufferDesc bd{}; bd.size=uint64_t(ws.tw)*ws.th*sizeof(uint32_t); bd.stride=sizeof(uint32_t);
    bd.usage=RHI::Usage::DEFAULT; bd.bindFlags=RHI::BindFlag::SHADER_RESOURCE|RHI::BindFlag::UNORDERED_ACCESS;
    bd.miscFlags=RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    if (!gfx.CreateBuffer(bd,ws.list)) { LOG_ERROR("RTDIPass: list allocation failed"); return; }
    bd.size=CounterCount*sizeof(uint32_t);
    if (!gfx.CreateBuffer(bd,ws.counter)) { LOG_ERROR("RTDIPass: counter allocation failed"); return; }
    bd.size=3*sizeof(uint32_t);
    if (!gfx.CreateBuffer(bd,ws.args)) { LOG_ERROR("RTDIPass: args allocation failed"); return; }
    ws.listState=ws.counterState=ws.argsState=RHI::ResourceState::UNDEFINED;
    bd.size=CounterCount*sizeof(uint32_t); bd.usage=RHI::Usage::READBACK; bd.bindFlags=RHI::BindFlag::NONE;
    if (main)   // the [RTDI] lines are the main view's
        for (uint32_t i=0;i<RHI::kFramesInFlight;++i)
            if (!gfx.CreateBuffer(bd,m_readback[i]) || !(m_mapped[i]=static_cast<const uint32_t*>(gfx.MapBuffer(m_readback[i]))))
            { LOG_ERROR("RTDIPass: counter readback failed"); return; }
    // The core's working set comes after the shell's; keep the order, the descriptor indices
    // follow it.
    if (!ws.core->Allocate(gfx,ws.tw,ws.th)) return;
    if (!(flags&1u))
    {
        ws.nrd=std::make_unique<NrdDenoiser>();
        // RELAX is NRD's denoiser for RTXDI signals; REBLUR sizes its blur from the light distance and
        // smears every hard shadow (-di_nrd_reblur).
        ws.nrd->SetKind(!m_relax ? NrdDenoiser::Kind::ReblurDiffuseSpecular
            : (m_nrdSplit ? NrdDenoiser::Kind::RelaxDiffuseSpecularSplit : NrdDenoiser::Kind::RelaxDiffuseSpecular));
        // FP32 illumination pools (default): the typed FP16 UAV store rounds toward zero, and RELAX's
        // temporal loop stores one freshly truncated value a frame (SchoolNight lost 1.3% to it, more
        // with a longer history).
        ws.nrd->SetDiFloat32Pool(true);
        if (!ws.nrd->Resize(gfx,ws.tw,ws.th))
        { LOG_ERROR("RTDIPass: %s initialization failed",m_relax?"RELAX_DIFFUSE_SPECULAR":"REBLUR_DIFFUSE_SPECULAR"); return; }
    }
    ws.resourcesReady=true; m_hist->reset=true;
    if (main) m_status.residentBytes=ws.core->ResidentBytes();
}

void RTDIPass::Shutdown(IGraphicsDevice& gfx)
{
    ReleaseWorkingSet(gfx,m_main); m_cb.Destroy(gfx); gfx.DestroyPipelineLayout(m_layout);
    for (auto& p:m_pso) gfx.DestroyPipelineState(p);
    if (m_main.core) m_main.core->ReleasePipelines(gfx);
    m_main.core.reset();
    m_main.active=false;
    m_retiredNrd.clear();   // after the GPU drain
    ++m_generation;         // the views' cores recorded with the pipelines just released
}
void RTDIPass::ReloadShaders(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred(); Shutdown(gfx); Init(gfx);
    if (m_wanted) { ApplyBackend(); BuildPipelines(); }   // in use: rebuilt now, not a frame late
    OnCompile(gfx);
}
const char* RTDIPass::GetInertReason() const
{
    const WorkingSet& ws=*m_ws;   // the bound view's
    if (!m_pso.back().IsValid() || !m_cb.IsValid()) return "pipeline";
    if (!ws.core) return "pipeline";
    if (const char* why=ws.core->InertReason()) return why;
    if (!ws.resourcesReady) return "resources";
    if (!(BoundFlags()&1u) && (!ws.nrd || !ws.nrd->IsReady())) return "NRD";
    const auto* result=m_graph.GetPhysicalTexture(m_result);
    if (!result || !result->IsValid()) return "result";
    return nullptr;
}
uint64_t RTDIPass::GetUpcomingOutputSrv() const
{ return GetInertReason()?0:m_gfx->GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(m_result)); }
void RTDIPass::SetFrameData(const SceneData& fd, bool active, uint32_t worldGrid,
                          uint32_t frame, float jitterX, float jitterY)
{
    // Reservoirs hold a light INDEX and re-evaluate every target from the current GPULight and
    // GBuffer each frame, so light motion / flicker, shadow slot reassignment, texture streaming
    // and skinned TLAS records do not invalidate them. Only a change of the light SET can remap
    // indices (Lights v2): the reservoirs then follow their lights through the key map and NRD
    // drops its history (it holds the old set's irradiance); before the map's ring exists the
    // whole history resets. -di_no_light_translation instead resets on a light-count change only
    // (which misses a light removed and another added in the same frame).
    WorkingSet& ws=*m_ws;   // the bound view's
    if (active!=ws.active) m_hist->reset=true;
    else if (!m_translateLights) { if (fd.lightCount!=ws.fd.lightCount) m_hist->reset=true; }
    else if (!fd.lightsIdentity) { if (fd.lightMapIdx) m_hist->nrdReset=true; else m_hist->reset=true; }
    ws.fd=fd; ws.active=active; ws.grid=worldGrid; ws.frame=frame; ws.jitterX=jitterX; ws.jitterY=jitterY;
}

RHI::CommandList RTDIPass::Execute(RHI::CommandList cl)
{
    WorkingSet& ws=*m_ws;   // the bound view's
    if (!ws.active || GetInertReason()) return cl;
    IGraphicsDevice& gfx=cl.GetDevice();
    const bool main=m_ws==&m_main;
    const uint32_t flags=BoundFlags();
    const bool rrRaw=main && m_rrRaw;   // ray reconstruction is the main view's
    for(size_t i=0;i<ws.tex.size();++i)
        if(ws.placedMask&(1u<<i)) gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr,&ws.tex[i]),cl);

    const auto slot=gfx.GetFrameIndex();
    if (main && m_primed[slot])   // the [RTDI] lines are the main view's
    {
        using namespace ReSTIR::Counters;
        const auto* s=m_mapped[slot];
        if(Sys::CommandLine::Get().Has("-restir_history_probe"))
            LOG_INFO("[RESTIR_HISTORY] pass=RTDI frame=%u reset=%u reusedTemporal=%u lightVanished=%u",
                m_readbackFrame[slot],m_readbackReset[slot],s[DI::ReusedTemporal],s[DI::LightVanished]);
        if((flags&(1u<<24)) && (s[DI::VisibilityChanges] || ws.frame%60==0))
            LOG_INFO("[RTDI_CONFIDENCE] frame=%u rays=%u visibilityChanges=%u",
                m_readbackFrame[slot],s[DI::ConfidenceRays],s[DI::VisibilityChanges]);
        float nrdHistoryMax=0; std::memcpy(&nrdHistoryMax,&s[DI::NrdHistoryMax],sizeof(float));
        if(ws.frame%30==0) LOG_INFO("%s eligible=%u visibility=%u selected=%u nonfinite=%u candidates=%u half=%u reusedTemporal=%u reusedSpatial=%u occluded=%u misNaN=%u mSum=%u nrdHistoryMax=%.2f nrdHistoryAvg=%.2f reusedSpatial2=%u spatialStage0=%u spatialStage1=%u rawMirrorPixels=%u boiled=%u spatialVisibility=%u temporalVisibility=%u reuse=%u feedback=%u visibilityMIS=%u resolveFallback=%u phase=%u backend=%s lightVanished=%u",
            m_shadow?"[RTDI_SHADOW]":"[RTDI]",
            s[DI::Eligible],s[DI::Visibility],s[DI::Selected],s[DI::NonFinite],s[DI::Candidates],(flags&4u)?0:1,
            s[DI::ReusedTemporal],s[DI::ReusedSpatial],s[DI::Occluded],s[DI::MisNaN],s[DI::MSum],nrdHistoryMax,
            s[DI::NrdHistoryCount]?double(s[DI::NrdHistorySum])/s[DI::NrdHistoryCount]:0.0,s[DI::ReusedSpatial2],
            s[DI::SpatialStage0],s[DI::SpatialStage1],s[DI::RawMirrorPixels],s[DI::Boiled],s[DI::SpatialVisibility],
            s[DI::TemporalVisibility],(flags&8u)?1u:0u,ws.core->Feedback()?1u:0u,ws.core->VisibilityMIS()?1u:0u,
            s[DI::ResolveFallback],ws.phase,m_status.effectiveName,s[DI::LightVanished]);
        if(s[DI::MisNaN]) LOG_ERROR("RTDIPass%s: invalid MIS weights=%u",m_shadow?" (shadow)":"",s[DI::MisNaN]);
        if(s[DI::NonFinite]) LOG_ERROR("RTDIPass%s: nonfinite pixels=%u",m_shadow?" (shadow)":"",s[DI::NonFinite]);
        if((flags&(1u<<18)) && ws.frame%30==0)
        {
            float red=0,green=0,spec=0,nv=0,energy=0;
            std::memcpy(&red,&s[17],4); std::memcpy(&green,&s[18],4);
            std::memcpy(&spec,&s[21],4); std::memcpy(&nv,&s[22],4); std::memcpy(&energy,&s[23],4);
            LOG_INFO("[RTDI_BOUNDARY] redError=%.8f greenError=%.8f redCount=%u greenCount=%u specChroma=%.8f maxNdotV=%.8f diffuseEnergyError=%.8f",
                red,green,s[19],s[20],spec,nv,energy);
        }
    }
    auto index=[&](uint64_t h){return gfx.DescriptorHeapIndexFromGpuHandle(h);};
    auto graphIndex=[&](RG::RGTextureHandle h){return index(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(h)));};
    Constants cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),XMMatrixTranspose(XMMatrixInverse(nullptr,XMLoadFloat4x4(&ws.fd.viewProj))));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.worldToView),XMMatrixTranspose(XMLoadFloat4x4(&ws.fd.view)));
    std::memcpy(cb.camera,&ws.fd.camera,12); cb.rayTMax=m_rayTMax;
    cb.width=ws.w; cb.height=ws.h; cb.traceW=ws.tw; cb.traceH=ws.th;
    cb.depthIdx=graphIndex(m_depth); cb.normalIdx=graphIndex(m_normal); cb.albedoIdx=graphIndex(m_albedo); cb.surfaceIdx=graphIndex(m_surface);
    cb.extraIdx=graphIndex(m_extra); cb.velocityIdx=graphIndex(m_velocity); cb.materialsIdx=ws.fd.materialsIdx; cb.lightsIdx=ws.fd.lightsIdx;
    cb.worldGrid=ws.grid; cb.lightCount=ws.fd.lightCount; cb.alphaInstanceCount=ws.fd.instanceCount; cb.frame=ws.frame;
    cb.diffUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[DiffIn])); cb.specUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[SpecIn]));
    cb.normalUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[Normal])); cb.viewZUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[ViewZ]));
    cb.mvUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[MV])); cb.flags=flags;
    if(m_relax) cb.flags|=1u<<27; // RELAX front/back-end packing
    cb.diffSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[ws.nrd?DiffOut:DiffIn]));
    cb.specSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[ws.nrd?SpecOut:SpecIn]));
    cb.rawSpecSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[SpecIn])); cb.mirrorRoughness=m_mirrorRoughness;
    cb.listUav=index(gfx.GetBufferUAVGpuHandle(ws.list)); cb.listSrv=index(gfx.GetBufferSRVGpuHandle(ws.list));
    cb.counterUav=index(gfx.GetBufferUAVGpuHandle(ws.counter)); cb.argsUav=index(gfx.GetBufferUAVGpuHandle(ws.args));
    cb.historyReset=m_hist->reset;
    // This frame's 2x2 phase, a 4-step Halton-ordered rotation: (0,0) (1,1) (1,0) (0,1). 0 unless
    // the phase rotates (never at full resolution, so never in the calibration arm).
    static constexpr uint32_t kPhaseOrder[4]={0u,3u,1u,2u};
    ws.phaseRotating=m_rotatePhase && m_temporalAA && !(flags&4u);
    ws.phase=ws.phaseRotating?kPhaseOrder[ws.frame&3u]:0u;
    cb.tracePhase=ws.phase; cb.flags2=m_flags2|(rrRaw?8u:0u);   // bit 3 under RR: the light distance (kDIFlags2RRDistance)
    if(m_translateLights && !ws.fd.lightsIdentity && ws.fd.lightMapIdx)
    {
        cb.lightMapIdx=ws.fd.lightMapIdx; cb.prevLightCount=ws.fd.prevLightCount;
        // A light-set change is rare: say which frame re-indexed its history.
        LOG_INFO("[RTDI_LIGHTMAP] frame=%u previous=%u lights=%u reset=%u",ws.frame,ws.fd.prevLightCount,ws.fd.lightCount,m_hist->reset?1u:0u);
    }
    if(flags&512u)
    {
        cb.confidenceUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[Confidence]));
        cb.confidenceSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[Confidence]));
    }
    cb.alphaTestEnabled=ws.fd.alphaVA!=0;
    // The core's fields: its reuse-chain flag bits, reservoir indices, caps,
    // the reprojection deltas.
    RTDI::SceneInputs scene;
    scene.lightsIdx=ws.fd.lightsIdx; scene.lightCount=ws.fd.lightCount;
    scene.prevLightsIdx=ws.fd.prevLightsIdx; scene.prevLightCount=ws.fd.prevLightCount;
    scene.lightMapIdx=cb.lightMapIdx;
    scene.lightPdfIdx=m_lightPdfIdx; scene.lightPdfSize=m_lightPdfSize; scene.lightPdfMips=m_lightPdfMips;
    std::memcpy(scene.view,&ws.fd.view,sizeof(scene.view)); std::memcpy(scene.camera,&ws.fd.camera,sizeof(scene.camera));
    scene.rayTMax=m_rayTMax; scene.renderW=ws.w; scene.renderH=ws.h;
    RTDI::FillContext fill;
    fill.shellFlags=flags; fill.phase=ws.phase; fill.frame=ws.frame; fill.reset=m_hist->reset;
    fill.jitterX=ws.jitterX; fill.jitterY=ws.jitterY; fill.traceW=ws.tw; fill.traceH=ws.th;
    fill.scene=&scene;
    ws.core->FillConstants(gfx,cb,fill);
    // Each dispatch's constants occupy immutable, 256-byte-aligned storage.
    // Rewriting a single mapped CB after recording would change earlier stages.
    auto& constants=*m_cb.Current(gfx);
    constants.stages[0].values=cb;
    constants.stages[1].values=cb;
    constants.stages[2].values=cb;
    ws.core->PatchStages(gfx,constants);
    // -restir_cb_digest (diagnostic): one hash of everything the GPU is told
    // this frame - the three stage blocks - so a refactor can prove it records
    // the same work frame by frame (tools/restir_identity.py digest).
    static const bool s_cbDigest=Sys::CommandLine::Get().Has("-restir_cb_digest");
    if(s_cbDigest)
    {
        // The three Constants, not the 512-byte stages: their tails are padding
        // nobody writes. Copied out first - the block itself is write-combined.
        Constants copy[3]={constants.stages[0].values,constants.stages[1].values,constants.stages[2].values};
        LOG_INFO("[RESTIR_CB] pass=RTDI frame=%u hash=%016llx flags=%08x",ws.frame,
            static_cast<unsigned long long>(Sys::WordHash64(copy,sizeof(copy))),copy[0].flags);
    }
    auto bind=[&] {
        namespace S=ReSTIR::ShellSlots;
        // The shell layout, through the first entry's pipeline: every shell and
        // core pipeline is built on it, so the arguments hold across their binds.
        gfx.BindComputePipelineState(m_pso[0],cl); gfx.SetComputeRootCBV(S::CB,m_cb.CurrentBuffer(gfx),0,cl);
        gfx.SetComputeRootSRVAddress(S::TLAS, ws.fd.tlasVA, cl); gfx.SetComputeRootSRVAddress(S::Instances, ws.fd.instanceVA, cl);
        gfx.SetComputeRootSRVAddress(S::Alpha, ws.fd.alphaVA, cl);
        gfx.SetComputeDescriptorTable(S::Buffers,ws.fd.buffers,cl); gfx.SetComputeDescriptorTable(S::Textures,gfx.GetBindlessTextureTableGpuHandle(),cl);
        gfx.SetComputeDescriptorTable(S::Result,gfx.GetTextureUAVGpuHandle(*m_graph.GetPhysicalTexture(m_result)),cl);
    };
    auto texState=[&](size_t i,RHI::ResourceState state) {
        if(ws.states[i]!=state) gfx.PushBarrier(RHI::GPUBarrier::Image(&ws.tex[i],ws.states[i],state),cl);
        ws.states[i]=state;
    };
    auto bufState=[&](RHI::GPUBuffer& b,RHI::ResourceState& before,RHI::ResourceState after) {
        if(before!=after) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&b,before,after),cl); before=after;
    };
    auto uavBarrier=[&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(),cl); };
    auto dispatch=[&](size_t p,uint32_t x,uint32_t y,const char* name) {
        const auto timer=gfx.BeginGPUTimestamp(cl,name); gfx.BindComputePipelineState(m_pso[p],cl); gfx.DispatchCompute(x,y,1,cl); gfx.EndGPUTimestamp(cl,timer);
    };
    RTDI::CoreFrame frame;
    frame.gfx=&gfx; frame.cl=cl;
    frame.args=&ws.args; frame.cb=&m_cb; frame.frame=ws.frame; frame.reset=m_hist->reset;
    frame.jitterX=ws.jitterX; frame.jitterY=ws.jitterY; frame.phase=ws.phase; frame.regions=m_regions;
    bind();
    for(size_t i=0;i<ws.tex.size();++i) if(ws.tex[i].IsValid()) texState(i,RHI::ResourceState::UNORDERED_ACCESS);
    bufState(ws.list,ws.listState,RHI::ResourceState::UNORDERED_ACCESS);
    bufState(ws.counter,ws.counterState,RHI::ResourceState::UNORDERED_ACCESS);
    bufState(ws.args,ws.argsState,RHI::ResourceState::UNORDERED_ACCESS);
    ws.core->PreShell(frame);
    dispatch(0,1,1,m_regions->clear); uavBarrier();
    dispatch(1,(ws.tw+7)/8,(ws.th+7)/8,m_regions->classify); uavBarrier();
    if(flags&512u) texState(Confidence,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    dispatch(2,1,1,m_regions->prepare); uavBarrier();
    bufState(ws.list,ws.listState,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    bufState(ws.args,ws.argsState,RHI::ResourceState::INDIRECT_ARGUMENT);
    ws.core->Record(frame);
    texState(DiffIn,RHI::ResourceState::SHADER_RESOURCE_COMPUTE); texState(SpecIn,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    texState(Normal,RHI::ResourceState::SHADER_RESOURCE_COMPUTE); texState(ViewZ,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    if(flags&512u) texState(Confidence,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    if(ws.nrd)
    {
        NrdDenoiser::Params np{}; std::memcpy(np.worldToView,&ws.fd.view,64); std::memcpy(np.viewToClip,&ws.fd.projection,64);
        np.width=ws.tw; np.height=ws.th; np.frameIndex=ws.frame; np.resetHistory=m_hist->reset||m_hist->nrdReset;
        // Like every other NRD user: beyond denoisingRange NRD writes nothing and the (aliased)
        // outputs would keep another pass's bytes.
        // Anti-firefly: off, every jitter-locked lamp-highlight spike on thin glossy geometry stays in
        // RELAX's history for 10-25 frames (with the DI specular AA and the fixed trace phase it took
        // the SchoolNight DLSS flicker from 8713 to 904 pixels). On the diffuse lobe it clipped ~3.4%
        // of the occluded multi-light fixture, so the split RELAX (default) runs it on the specular
        // denoiser only; diffuse is opt-in (-di_nrd_antifirefly). -di_no_nrd_antifirefly turns it off
        // everywhere. REBLUR and the combined RELAX (-di_nrd_combined) keep one switch, on by default.
        // The specular one is a setting too (Settings::specularAntiFirefly, engine.json
        // restirDiSpecAntiFirefly): it clips small bright highlights (metal ~16% low against the path
        // tracer on SchoolNight).
        const bool antiFireflyOptIn=Sys::CommandLine::Get().Has("-di_nrd_antifirefly");
        const bool antiFireflyOff=(flags&64u)!=0 || !m_specAntiFireflySetting;
        const bool split=m_relax && m_nrdSplit;
        np.denoisingRange=ws.fd.farZ*0.99f; np.diHitDistScaleA=m_rayTMax*m_nrdHitDistScale;
        np.diAntiFirefly=split ? (antiFireflyOptIn && !antiFireflyOff) : (!antiFireflyOff || antiFireflyOptIn);
        np.diSpecularAntiFirefly=!antiFireflyOff;
        np.diHistoryCounts=(flags&128u)!=0;
        if(::Settings::Pass::rtdiNrdHitDistScaleGiven() && ws.frame%60==0)
            LOG_INFO("[RTDI_HITDIST] frame=%u producerA=%.6f denoiserA=%.6f scale=%.6f",ws.frame,m_rayTMax,np.diHitDistScaleA,m_nrdHitDistScale);
        np.historyConfidenceAvailable=(flags&512u)!=0;
        np.diTemporalStabilization=!Sys::CommandLine::Get().Has("-di_nrd_no_stabilization");
        // The pre-pass is a wide blur for very noisy single-sample input. With reuse feeding RELAX it
        // only softens hard shadows, so it is off there by default (-di_nrd_prepass turns it on;
        // -di_nrd_no_prepass forces it off).
        const auto& command=Sys::CommandLine::Get();
        np.diPrepass=command.Has("-di_nrd_prepass") ||
            (!command.Has("-di_nrd_no_prepass") && !(m_relax && (flags&8u)));
        np.diBlurRadiusScale=(flags&(1u<<22)) && !(flags&4u)?0.5f:1.f;
        // Diagnostic multiplier in full-resolution pixels, applied after the
        // trace-stride conversion so full/half arms use the same footprint.
        const float requestedRadius=Sys::CommandLine::Get().GetFloat("-di_nrd_radius_scale",1.f);
        const float radius=std::isfinite(requestedRadius)?std::clamp(requestedRadius,0.5f,1.f):1.f;
        np.diBlurRadiusScale*=radius;
        if(Sys::CommandLine::Get().Has("-di_nrd_radius_scale") && ws.frame%60==0)
            LOG_INFO("[RTDI_RADIUS] frame=%u multiplier=%.6f traceScale=%.6f",ws.frame,radius,np.diBlurRadiusScale);
        // TAA jitter j moves the sampled point by -j; the trace phase moves it by +phase. While the
        // phase rotates, NRD gets the half-resolution sample's true offset from its texel centre,
        // (j-phase+0.5)/2, which stays inside NRD's [-0.5,0.5] ((j-phase)/2 does not, and NRD refuses
        // it). A fixed phase passes j scaled to the trace resolution.
        const float scale=(flags&4u)?1.f:.5f;
        if(ws.phaseRotating)
        {
            np.jitter[0]=(ws.jitterX-float(ws.phase&1u)+.5f)*scale;
            np.jitter[1]=(ws.jitterY-float((ws.phase>>1)&1u)+.5f)*scale;
        }
        else { np.jitter[0]=ws.jitterX*scale; np.jitter[1]=ws.jitterY*scale; }
        auto binding=[&](nrd::ResourceType type,size_t i,RHI::ResourceState state) {
            return NrdDenoiser::Binding{type,{&ws.tex[i],state}};
        };
        const auto srv=RHI::ResourceState::SHADER_RESOURCE_COMPUTE, uav=RHI::ResourceState::UNORDERED_ACCESS;
        std::vector<NrdDenoiser::Binding> bindings={
            binding(nrd::ResourceType::IN_MV,MV,uav),
            binding(nrd::ResourceType::IN_NORMAL_ROUGHNESS,Normal,srv),
            binding(nrd::ResourceType::IN_VIEWZ,ViewZ,srv),
            binding(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,DiffIn,srv),
            binding(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,SpecIn,srv),
            binding(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST,DiffOut,uav),
            binding(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST,SpecOut,uav)};
        if(np.historyConfidenceAvailable)
        {
            bindings.push_back(binding(nrd::ResourceType::IN_DIFF_CONFIDENCE,Confidence,srv));
            bindings.push_back(binding(nrd::ResourceType::IN_SPEC_CONFIDENCE,Confidence,srv));
        }
        // Named after the denoiser that actually runs (RELAX by default).
        const auto nrdTimer=gfx.BeginGPUTimestamp(cl,m_relax?m_regions->relax:m_regions->reblur);
        if(!ws.nrd->Denoise(gfx,cl,np,bindings.data(),static_cast<uint32_t>(bindings.size()))) LOG_ERROR("RTDIPass: NRD dispatch failed");
        gfx.EndGPUTimestamp(cl,nrdTimer); gfx.BindDescriptorHeaps(cl); bind();
        texState(DiffOut,RHI::ResourceState::SHADER_RESOURCE_COMPUTE); texState(SpecOut,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }
    dispatch(3,(ws.w+7)/8,(ws.h+7)/8,m_regions->resolve);
    if (main)
    {
        bufState(ws.counter,ws.counterState,RHI::ResourceState::COPY_SRC);
        gfx.CopyBuffer(ws.counter,m_readback[slot],CounterCount*sizeof(uint32_t),cl); m_primed[slot]=true;
        m_readbackFrame[slot]=ws.frame; m_readbackReset[slot]=m_hist->reset;
    }
    m_hist->reset=m_hist->nrdReset=false;
    return cl;
}

void RTDIPass::OnService(RG::ServiceContext& c)
{
    for (size_t i=0;i<m_retiredNrd.size();)
    {
        if (--m_retiredNrd[i].second==0) m_retiredNrd.erase(m_retiredNrd.begin()+static_cast<ptrdiff_t>(i));
        else ++i;
    }
    const ServiceRequest& r=m_request;
    if (!m_shadow)
    {
        SetBackend(r.backend);
        SetRayReconstruction(r.raw);
        SetSettings(r.settings);
        EnsureResources(r.wanted,r.retain);
        c.board.diEngineRuns=GetCoreComposition()==ReSTIR::DIComposition{};
        c.board.diHistoryReset=r.wanted && HistoryResetPending();
    }
    else
    {
        // Compare mode: the shadow instance runs the other core.
        SetBackend(c.board.diEngineRuns ? ReSTIR::Backend::Rtxdi : ReSTIR::Backend::Engine);
        SetSettings(r.settings);
        EnsureResources(r.wanted,false);
    }
    c.board.diLightTables=c.board.diLightTables || (r.wanted && WantsLightTables());
}
