#include "RenderGraph/RenderPass/RTGIPass.h"
#include "Settings/PassSettings.h"     // pass.rtgi.*
#include "RenderGraph/RenderPass/RTGIEngineCore.h"
#include "RenderGraph/RenderPass/RTGIRtxdiCore.h"
#include "RenderGraph/RenderPass/RTDIPass.h"
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderReflection.h"
#include "Graphics/ReSTIRTransientTextures.h"
#include "RenderGraph/PassTextureNames.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/WordHash.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <string>

namespace RTGI
{
const RegionNames kMainRegions{"RTGI.Clear","RTGI.EmitterArea","RTGI.EmitterCdf","RTGI.Trace","RTGI.Temporal",
    "RTGI.Spatial","RTGI.Shade","RTGI.Prepare","RTGI.Boiling","RTGI.Final","RTGI.RELAX","RTGI.REBLUR","RTGI.Resolve",
    "RTGI.Sharc.Clear","RTGI.Sharc.Update","RTGI.Sharc.Resolve"};
const RegionNames kShadowRegions{"RTGI.Shadow.Clear","RTGI.Shadow.EmitterArea","RTGI.Shadow.EmitterCdf",
    "RTGI.Shadow.Trace","RTGI.Shadow.Temporal","RTGI.Shadow.Spatial","RTGI.Shadow.Shade","RTGI.Shadow.Prepare",
    "RTGI.Shadow.Boiling","RTGI.Shadow.Final","RTGI.Shadow.RELAX","RTGI.Shadow.REBLUR","RTGI.Shadow.Resolve",
    "RTGI.Shadow.Sharc.Clear","RTGI.Shadow.Sharc.Update","RTGI.Shadow.Sharc.Resolve"};
} // namespace RTGI

RTGIPass::RTGIPass(RG::RenderGraph& graph, RG::RGTextureHandle albedo, RG::RGTextureHandle normal,
    RG::RGTextureHandle surface, RG::RGTextureHandle depth, RG::RGTextureHandle velocity, RG::RGTextureHandle result,
    bool shadow)
    :m_shadow(shadow),m_regions(shadow?&RTGI::kShadowRegions:&RTGI::kMainRegions),
     m_graph(graph),m_albedo(albedo),m_normal(normal),m_surface(surface),m_depth(depth),m_velocity(velocity),m_result(result) {}
RTGIPass::~RTGIPass() = default;
void RTGIPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_albedo); b.ReadSRV(m_normal); b.ReadSRV(m_surface); b.ReadSRV(m_depth); b.ReadSRV(m_velocity); b.WriteUAV(m_result);
    // The sky the misses sample (FrameData::skyIdx - SkyIBL's specular cube
    // while the atmosphere draws the sky; SkyIBLPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kSkySpecularCube });
}
void RTGIPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this, GetName(), this);
    if (view.Slot() == 0)
        m_ws = &m_main;
    else
    {
        ViewWork& work = view.Data<ViewWork>(this);
        work.owner = this;
        m_ws = &work.ws;
    }
}

void RTGIPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for(auto& tex:ws.tex) { if(tex.IsValid()) gfx.DestroyTexture(tex); tex={}; }
    if(ws.counter.IsValid()) gfx.DestroyBuffer(ws.counter);
    ws.counter={};
    if(ws.core) ws.core->Release(gfx);
    if(owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd),RHI::kFramesInFlight+1u);
    ws=WorkingSet{};
}

void RTGIPass::Init(IGraphicsDevice& gfx)
{
    ENGINE_CHECK(m_hist != nullptr, "%s: no view bound before Init (PassSet::SetView before its registration)",
                 GetName());
    m_gfx=&gfx;
    if(!gfx.GetCapabilities().inlineRaytracing) return;
    const auto& cmd=Sys::CommandLine::Get();
    // GI defaults to half the render width/height; Resolve reconstructs the
    // render-sized result. engine.json restirGiFullRes chooses full resolution
    // at runtime and restirGiDenoiser the denoiser (ApplySettingFlags);
    // -gi_fullres / -gi_halfres and -gi_no_nrd / -gi_nrd_reblur also set them.
    m_flags=(cmd.Has("-gi_constant_radiance")?2u:0u);
    // -gi_rr_raw: the GI reaches ray reconstruction undenoised (an A/B arm;
    // ApplySettingFlags).
    m_rrRawGiCli=cmd.Has("-gi_rr_raw");
    // engine.json restirGiSpatial=false sets flag 16 (-gi_no_spatial also sets it).
    if(cmd.Has("-gi_reject_all")) m_flags|=32u;
    if(cmd.Has("-gi_no_validation")) m_flags|=64u;
    if(cmd.Has("-gi_no_inverse_visibility")) m_flags|=128u; // A/B: canonical MIS without neighbour visibility
    if(cmd.Has("-gi_geometric_hitdist")) m_flags|=256u; // A/B: REBLUR hit distance from this frame's ray
    // RELAX_DIFFUSE is the default GI denoiser (the denoiser setting reblur,
    // -gi_nrd_reblur, selects REBLUR; ApplySettingFlags sets flag 512): REBLUR
    // sizes its kernel from the hit distance, and a ReSTIR reservoir's selected
    // connection is usually a long one, so it blurs contact shadows and corner
    // darkening away (~2.3x the low-frequency error on a Cornell box).
    if(::Settings::Pass::rtgiBoilingFilter()) m_flags|=1024u;     // opt-in RTXDI GI boiling filter
    if(::Settings::Pass::rtgiJacobianClamp()) m_flags|=2048u;     // opt-in RTXDI [1/3,3] Jacobian clamp
    if(cmd.Has("-gi_no_hit_emission")) m_flags|=4096u;   // diagnostic: drop emission at GI hits
    // RTXDI GI temporal: a per-pixel age limit in [maxAge/2, maxAge].
    if(!cmd.Has("-gi_no_random_age_cap")) m_flags|=32768u;
    if(cmd.Has("-restir_no_jitter_reprojection")) m_flags|=16384u;
    // A/B: the receiver horizon is the shading normal (no depth-reconstructed
    // geometric normal); the receiver is still the ray origin.
    if(cmd.Has("-gi_shading_horizon")) m_flags|=131072u;
    // Initial candidates: -gi_initial_candidates cosine rays and, with emitter
    // sampling, -gi_emitter_candidates emitter points, in one balance-heuristic
    // RIS stream (-gi_no_emitter_sampling is the off arm).
    if(!cmd.Has("-gi_no_emitter_sampling")) m_flags|=65536u;
    // The three counts follow engine.json (SetSettings; -gi_initial_candidates,
    // -gi_emitter_candidates and -gi_hit_exact_lights also set them).
    // Local lights at a GI hit: the exact sum up to restirGiHitExactLights,
    // beyond that pass.rtgi.hitLightSamples resampled picks, each with its
    // shadow ray.
    m_hitLightSamples=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiHitLightSamples(),1,16));
    if(cmd.Has("-gi_no_disocclusion_boost")) m_flags|=262144u;
    if(cmd.Has("-gi_temporal_1overM")) m_flags|=524288u;
    if(cmd.Has("-gi_no_coverage_filter")) m_flags|=1048576u; // A/B: raw per-pixel coverage
    // Per-receiver emitter pmf up to this many emitters (global power CDF above).
    // 256: with 387 emitters the per-receiver pmf makes FEWER candidates visible
    // than the global CDF (249k vs 302k) for +28 ms of Trace.
    m_localEmitterLimit=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiLocalEmitters(),0,4096));
    // raytracing.restirGiMaxDistance arrives with SetSettings (-gi_ray_tmax also
    // sets it); a reload keeps the distance last given.
    if(!m_haveCoreSettings) m_rayTMax=150.f;
    m_constantRadiance=std::max(0.f,cmd.GetFloat("-gi_constant_radiance",1.f));
    m_seed=static_cast<uint32_t>(cmd.GetInt("-seed",1));
    // SHaRC. engine.json restirGiSharc / restirGiSharcMode choose it at runtime
    // (SetSettings); -gi_sharc[=0|1] and -gi_sharc_mode=parity|multi also set them
    // (engine.json's boot load refuses a bad mode, exit 12).
    {
        // The gate (RTGI_HitRadiance): voxels up to 1 m, i.e. hits closer than
        // ~64 m at scene scale 50; entries above 2 samples (a compile-time
        // SHARC_SAMPLE_NUM_THRESHOLD: the Update / Resolve / query builds share it).
        m_sharcMaxVoxel=std::clamp(::Settings::Pass::rtgiSharcMaxVoxel(),.01f,1000.f);
        m_sharcMinSamples=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiSharcMinSamples(),0,64));
        m_sharcForceMiss=cmd.Has("-gi_sharc_force_miss");   // diagnostic: every query misses (= the analytic shading)
        m_sharcBound=cmd.Has("-gi_sharc_bound");            // diagnostic, the bound arm: no hit shading, no update
        m_sharcResampling=::Settings::Pass::rtgiSharcResampling();  // multi-bounce: SHaRC's cache resampling in the update
        m_sharcClearOnReset=cmd.Has("-gi_sharc_clear_on_history_reset");   // diagnostic: the lifecycle pack's reverse arm
        // The validation's reset of a sample the cache shaded (RTGI.cs.hlsl
        // GI_LUMINANCE_RESET): a relative luminance change above this resets its
        // history. .6 by default: the cache's estimate moves frame to frame, so the
        // analytic rule's .2 resets ~10x the histories (~1000 vs ~90 a frame; ~22 with
        // the cache off) at the same noise and energy. pass.rtgi.sharcCachedReset=.2
        // is that A/B arm.
        m_sharcCachedReset=std::clamp(::Settings::Pass::rtgiSharcCachedReset(),.01f,100.f);
        // The cache's shared -sharc_* flags (SharcCache.h); GI's capacity 2^21
        // (80 MiB): only the GI hits deposit and query. The PT core's update tile (5)
        // and 20-frame window stay GI's defaults: a 40-frame window or a 3x3 tile cuts
        // the luminance resets to ~1.1-1.5x the cache-off count and the noise a little
        // further, but doubles the lamp-off latency (~95 -> ~190 frames to 1%) or costs
        // 2.8x the update paths.
        SharcCache::Config defaults; defaults.capacityLog2=21;
        m_sharcConfig=SharcCache::FromCommandLine(defaults);
        m_sharcDefines.clear();          // rebuilt from these by BuildSharcPipelines
        m_sharcPipelinesFailed=false;    // a hot reload retries a failed build
    }
    // The reuse core: the engine's; a backend switch replaces it at the next
    // EnsureResources. It owns bit 8 (reuse) of the flag word.
    ApplySettingFlags();
    m_main.core=std::make_unique<RTGI::EngineCore>();
    if(m_main.core->Reuse()) m_flags|=8u;
    m_coreBackend=ReSTIR::Backend::Engine;
    m_backendDirty=true;
    ++m_generation;   // a secondary view's core records with this one's pipelines
    // The ReSTIR shells' common layout (ReSTIR::CreateShellLayout): b0, the
    // TLAS / instance / alpha-record root SRVs, the bindless tables, u0, a
    // core's own b1 (the RTXDI core's constants), s1.
    if (!ReSTIR::CreateShellLayout(gfx,m_layout,GetName()))
    { LOG_ERROR("RTGIPass: pipeline layout failed"); return; }
    // The pipelines are built on first use (BuildPipelines, from EnsureResources).
    m_pipelinesFailed=false;
    if(!m_cb.Create(gfx,"RTGI.Constants")) LOG_ERROR("RTGIPass: constants failed");
}
bool RTGIPass::BuildPipelines()
{
    if(m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines()) return true;
    if(m_pipelinesFailed || !m_layout.IsValid() || !m_gfx || !m_main.core) return false;
    IGraphicsDevice& gfx=*m_gfx;
    const auto start=std::chrono::steady_clock::now();
    // The core's kernels are built on a thread of their own while the shell's
    // are built here, each set's pipelines concurrently: a RayQuery kernel's
    // pipeline is a driver compile on every run (Trace ~270 ms;
    // ComputePipelineBatch). Each half writes only its own pipelines.
    std::future<bool> core=std::async(std::launch::async,[&]{ return m_main.core->BuildPipelines(gfx,m_layout); });
    static constexpr const char* kEntries[]={"Clear","Trace","Resolve","EmitterArea","EmitterCdf"};
    static_assert(std::size(kEntries)==std::tuple_size_v<decltype(m_pso)>);
    const bool shellOk=m_pso.back().IsValid() || ComputePipelineBatch::BuildEntries(gfx,m_layout,"shaders/RTGI.cs.hlsl",
                                                                                   kEntries,m_pso.size(),m_pso.data(),"RTGIPass");
    const bool coreOk=core.get();
    if(!shellOk || !coreOk) { m_pipelinesFailed=true; return false; }
    LOG_INFO("RTGIPass: pipelines built on first use (%.0f ms)",
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    // A hot reload (ReloadShaders -> Init) rebuilds the cache's kernels now too
    // - only where they can run (the update needs 64-bit heap atomics: its
    // pipeline would fail, with an error, on a device without them).
    if(m_sharcWanted && !SharcCache::InertReason(*m_gfx,true)) BuildSharcPipelines();
    return true;
}
namespace
{
// RTGI.cs.hlsl's cbuffer Params in an RTGI_SHARC build: 416 bytes, then the
// SHaRC tail RTGI::Constants mirrors.
bool SharcLayoutMatches(const std::vector<uint8_t>& dxil)
{
    ShaderReflect::Reflection r;
    if(!ShaderReflect::Reflect(dxil.data(),dxil.size(),r)) { LOG_ERROR("RTGIPass: SHaRC Trace has no reflection"); return false; }
    for(const auto& cb:r.cbuffers)
    {
        if(cb.name!="Params") continue;
        uint32_t hashOffset=~0u;
        for(const auto& v:cb.vars) if(v.name=="sharcHashUav") hashOffset=v.offset;
        if(cb.sizeBytes==sizeof(RTGI::Constants) && hashOffset==RTGI::kConstantsOffBytes) return true;
        LOG_ERROR("RTGIPass: SHaRC cbuffer Params is %u bytes with sharcHashUav at %u; RTGI::Constants is %zu with it at %zu",
            cb.sizeBytes,hashOffset,sizeof(RTGI::Constants),RTGI::kConstantsOffBytes);
        return false;
    }
    LOG_ERROR("RTGIPass: SHaRC Trace has no cbuffer Params");
    return false;
}
} // namespace
bool RTGIPass::BuildSharcPipelines()
{
    if(m_sharcPipelinesFailed || !m_gfx || !m_layout.IsValid() || !m_main.core) return false;
    IGraphicsDevice& gfx=*m_gfx;
    // One define list for every build, so the Update, Resolve and query agree
    // on the maturity threshold (SHaRC compiles it in).
    if(m_sharcDefines.empty())
    {
        m_sharcDefines.push_back({"SHARC_SAMPLE_NUM_THRESHOLD",std::to_string(m_sharcMinSamples)});
        m_sharcDefines.push_back({"SHARC_ENABLE_CACHE_RESAMPLING",m_sharcResampling?"1":"0"});
        char reset[32];
        std::snprintf(reset,sizeof(reset),"%.4f",m_sharcCachedReset);
        m_sharcDefines.push_back({"RTGI_SHARC_CACHED_RESET",reset});
    }
    auto build=[&](const char* entry,const std::vector<DxcCompiler::Define>& defines,
                   RHI::PipelineState& out,bool layout) {
        if(out.IsValid()) return true;
        // 16-bit types (SHaRC's fp16 storage) on the engine's HLSL; the recipe records
        // the flag, so a hot reload compiles these exactly the same way.
        const auto dxil=ShaderDiskCache::LoadOrCompile("shaders/RTGISharc.cs.hlsl",entry,RHI::ShaderStage::CS,defines,
                                                       "RTGIPass",ShaderDiskCache::CompileFlags{false,true});
        const bool ok=!dxil.empty() && (!layout || SharcLayoutMatches(dxil)) &&
                      ComputePipelineBatch::Create(gfx,m_layout,&dxil,1,&out);
        if(!ok) { LOG_ERROR("RTGIPass: SHaRC pipeline %s failed",entry); gfx.DestroyPipelineState(out); }
        return ok;
    };
    bool ok=build("Trace",m_sharcDefines,m_traceSharc,true);
    if(!m_shadow)   // the shadow pass only queries the main pass's cache
    {
        ok=ok && build("RTGISharcClear",m_sharcDefines,m_sharcPso[SharcClear],false);
        std::vector<DxcCompiler::Define> update=m_sharcDefines;
        update.push_back({"SHARC_UPDATE","1"});
        ok=ok && build("RTGISharcUpdate",update,m_sharcPso[SharcUpdate],false);
        ok=ok && build("RTGISharcResolve",m_sharcDefines,m_sharcPso[SharcResolve],false);
    }
    // The core's validation build (the engine core's Temporal, the RTXDI
    // core's Prepare): without it a cached sample would be re-shaded
    // analytically at every validation, so a failure keeps the cache off
    // (locked until the next Init, like the shell's own pipelines).
    ok=ok && m_main.core->BuildSharcPipelines(gfx,m_layout,m_sharcDefines) && m_main.core->HasSharcPipelines();
    if(!ok) { m_sharcPipelinesFailed=true; ReleaseSharcPipelines(); return false; }
    return true;
}
void RTGIPass::ReleaseSharcPipelines()
{
    if(!m_gfx) return;   // nothing is built before Init
    for(auto& p:m_sharcPso) m_gfx->DestroyPipelineState(p);
    m_gfx->DestroyPipelineState(m_traceSharc);
    if(m_main.core) m_main.core->ReleaseSharcPipelines(*m_gfx);
}
bool RTGIPass::SharcRuns() const
{
    if(!m_sharcActive || !m_traceSharc.IsValid() || !m_main.core || !m_main.core->HasSharcPipelines()) return false;
    if(!SharcSourceCache()->IsAllocated()) return false;
    return m_shadow || (m_sharcPso[SharcClear].IsValid() && m_sharcPso[SharcUpdate].IsValid() && m_sharcPso[SharcResolve].IsValid());
}
void RTGIPass::ApplySharc()
{
    // At the service point, after the shell and the core have their pipelines
    // and working set (EnsureResources): whether the cache runs, and why not.
    // Never inert: without the cache GI shades every hit analytically.
    const bool wasActive=m_sharcActive;
    const char* why=nullptr;
    // The device and the unpacked headers do not change during a run: asked
    // once (the header probe is a file-system call).
    if(!m_sharcInertChecked) { m_sharcInert=SharcCache::InertReason(*m_gfx,true); m_sharcInertChecked=true; }
    if(!m_sharcWanted) why=m_sharcPinned?"off (-gi_sharc=0)":"off (engine.json restirGiSharc, -gi_sharc)";
    else if(m_shadow && (!m_sharcSource || !m_sharcSource->m_sharcActive)) why="the main pass runs no cache";
    else if(m_sharcInert) why=m_sharcInert;
    else if(!m_pso.back().IsValid() || !m_main.core || !m_main.core->HasPipelines()) why="shell pipeline";
    else if(m_sharcPipelinesFailed || !BuildSharcPipelines()) why="pipeline";
    else if(!m_shadow && !m_sharc.Allocate(*m_gfx,m_sharcConfig.capacityLog2)) why="allocation";
    m_sharcActive=why==nullptr;
    m_sharcWhy=why?why:"selected";
    if(!m_sharcActive && !m_shadow && m_sharc.IsAllocated())
    {
        const uint64_t bytes=m_sharc.ResidentBytes();
        m_sharc.Release(*m_gfx);
        m_sharc.RequestClear("allocate");
        LOG_INFO("[RTGI_SHARC] released bytes=%llu reason=%s",static_cast<unsigned long long>(bytes),m_sharcWhy);
    }
    // A sample carries the cached bit: the cache coming or going starts a
    // fresh reservoir history (like a backend switch).
    if(m_sharcActive!=wasActive) ResetHistory();
    if(m_sharcActive!=wasActive || m_sharcLogPending || (m_sharcActive && m_sharcMode!=m_sharcLoggedMode))
    {
        m_sharcLogPending=false;
        m_sharcLoggedMode=m_sharcMode;
        LOG_INFO("[RTGI_SHARC] stage=%s enabled=%u reason=%s mode=%s capacity=%u bytes=%llu sceneScale=%.1f updateTile=%u updateBounces=%u accumulationFrames=%u staleFrames=%u radianceScale=%.0f maxVoxel=%.2f minSamples=%u cachedReset=%.2f resampling=%u tier=%u%s",
            m_shadow?"GI.Shadow":"GI",m_sharcActive?1u:0u,m_sharcWhy,m_sharcMode==1?"multi":"parity",
            m_sharcActive?(1u<<m_sharcConfig.capacityLog2):0u,
            static_cast<unsigned long long>(m_sharcActive&&!m_shadow?SharcCache::BytesFor(m_sharcConfig.capacityLog2):0ull),
            m_sharcConfig.sceneScale,m_sharcConfig.updateTile,m_sharcConfig.updateBounces,m_sharcConfig.accumulationFrames,
            m_sharcConfig.staleFrames,m_sharcConfig.radianceScale,m_sharcMaxVoxel,m_sharcMinSamples,m_sharcCachedReset,
            m_sharcResampling?1u:0u,m_tier,m_tierForced?" tierForced=1":"");
    }
}
RTGIPass::SharcStatus RTGIPass::GetSharcStatus() const
{
    SharcStatus s;
    s.requested=m_sharcWanted; s.effective=m_sharcActive; s.reason=m_sharcWhy; s.mode=m_sharcMode;
    s.bytes=m_shadow?0ull:m_sharc.ResidentBytes(); s.clears=m_sharc.Clears();
    return s;
}
void RTGIPass::SetBackend(ReSTIR::Backend requested)
{
    if(!ReSTIR::IsLegal(ReSTIR::Stage::GI,requested)) requested=ReSTIR::Backend::Engine;
    if(requested!=m_requested) { m_requested=requested; m_backendDirty=true; }
}
void RTGIPass::ApplyBackend()
{
    if(!m_backendDirty) return;
    m_backendDirty=false;
    m_status.requested=m_requested;
    m_status.requestedName=ReSTIR::BackendName(m_requested);
    // The RTXDI core when this build and device can run it; otherwise the
    // engine core honours the request LOUDLY, with RtxdiInertReason's reason.
    ReSTIR::Backend effective=ReSTIR::Backend::Engine;
    const char* reason="selected";
    if(m_requested!=ReSTIR::Backend::Engine)
    {
        const bool native16=m_gfx && m_gfx->GetCapabilities().native16BitShaderOps;
        if(const char* inert=ReSTIR::RtxdiInertReason(ReSTIR::Stage::GI,native16)) reason=inert;
        else effective=ReSTIR::Backend::Rtxdi;
    }
    if(!m_main.core || effective!=m_coreBackend)
    {
        // The switch: the old core's working set and pipelines go, the new one
        // builds on first use (EnsureResources) and starts with an empty history; the
        // shell's set (NRD, the transient textures, the counters, the result) stays.
        // At the frame boundary: the GPU may still hold the old core's pipelines.
        std::unique_ptr<RTGI::Core> next;
        if(effective==ReSTIR::Backend::Rtxdi) next=RTGI::MakeRtxdiCore();
        if(!next && effective!=ReSTIR::Backend::Engine) { effective=ReSTIR::Backend::Engine; reason="rtxdi sdk not in this build"; }
        if(!next) next=std::make_unique<RTGI::EngineCore>();
        if(m_main.core && m_gfx)
        {
            m_gfx->WaitIdleAndReleaseDeferred();
            const uint64_t bytes=m_main.core->IsAllocated()?m_main.core->ResidentBytes():0;
            if(m_main.core->IsAllocated()) m_main.core->Release(*m_gfx);
            m_main.core->ReleasePipelines(*m_gfx);
            LOG_INFO("[RESTIR_SWITCH] pass=%s from=%s to=%s releasedBytes=%llu",GetName(),
                ReSTIR::BackendName(m_coreBackend),ReSTIR::BackendName(effective),static_cast<unsigned long long>(bytes));
        }
        m_main.core=std::move(next);
        m_coreBackend=effective;
        ++m_generation;   // the views' cores are made again with it
        if(m_haveCoreSettings) { bool reset=false; m_main.core->ApplySettings(m_coreSettings,reset); }
        m_flags=m_main.core->Reuse()?(m_flags|8u):(m_flags&~8u);
        ResetHistory();
    }
    m_status.effective=effective;
    m_status.effectiveName=ReSTIR::BackendName(effective);
    m_status.reason=reason;
    m_status.ignored=m_main.core ? m_main.core->IgnoredFlags() : "";
    m_logBackend=true;
}
void RTGIPass::ApplySettingFlags()
{
    // Under ray reconstruction RELAX keeps denoising the GI. The half-resolution
    // Resolve's 3x3 bilateral gather spreads each raw sample over ~6x6 render
    // pixels - correlated noise (neighbour rho ~0.76), which RR assumes it never
    // gets and keeps as boiling detail (over a third of wind-frozen foliage pixels
    // flicker). RELAX denoises at trace resolution BEFORE that upsample, as on the
    // NRD path: ~2.1% flicker for ~0.15 ms, where a full-resolution trace reaches
    // ~2.6% for ~3.5x the GI cost. DI differs: under RR it traces full resolution
    // undenoised. -gi_rr_raw hands RR the raw signal (the A/B arm).
    // The denoiser setting: Auto and RELAX are that rule, REBLUR swaps NRD's
    // kind, None is the raw signal with or without RR.
    using ReSTIR::Denoiser;
    const bool full=m_fullResSetting;
    m_flags=full?(m_flags|4u):(m_flags&~4u);
    const bool noNrd=m_denoiserSetting==Denoiser::None || (m_rrRaw && m_rrRawGiCli);
    m_flags=noNrd?(m_flags|1u):(m_flags&~1u);
    const bool relax=m_denoiserSetting!=Denoiser::Reblur;
    m_flags=relax?(m_flags|512u):(m_flags&~512u);
    const bool noSpatial=!m_spatialSetting;
    m_flags=noSpatial?(m_flags|16u):(m_flags&~16u);
}
uint32_t RTGIPass::ViewFlags() const
{
    // ApplySettingFlags without ray reconstruction's term.
    const bool noNrd=m_denoiserSetting==ReSTIR::Denoiser::None;
    return noNrd?(m_flags|1u):(m_flags&~1u);
}

std::unique_ptr<RTGI::Core> RTGIPass::MakeCore(ReSTIR::Backend backend)
{
    std::unique_ptr<RTGI::Core> core;
    if(backend==ReSTIR::Backend::Rtxdi) core=RTGI::MakeRtxdiCore();
    if(!core) core=std::make_unique<RTGI::EngineCore>();
    return core;
}

void RTGIPass::SetRayReconstruction(bool on)
{
    if(on==m_rrRaw) return;
    m_rrRaw=on;
    const uint32_t before=m_flags;
    ApplySettingFlags();
    if((before^m_flags)&(1u|4u)) { m_main.resourcesReady=false; ResetHistory(); }
    LOG_INFO("[RESTIR_RR] stage=%s rayReconstruction=%u nrd=%u fullResolution=%u",m_shadow?"GI.Shadow":"GI",
        on?1u:0u,(m_flags&1u)?0u:1u,(m_flags&4u)?1u:0u);
}
void RTGIPass::LogBackend() const
{
    LOG_INFO("[RESTIR_BACKEND] stage=%s requested=%s effective=%s reason=%s pipelineMs=%.1f residentBytes=%llu ignored=%s",
        m_shadow?"GI.Shadow":"GI",m_status.requestedName,m_status.effectiveName,m_status.reason,
        m_status.pipelineMs,static_cast<unsigned long long>(m_status.residentBytes),
        m_status.ignored.empty()?"-":m_status.ignored.c_str());
}
void RTGIPass::EnsureResources(bool wanted,bool retain)
{
    m_wanted=wanted;
    if(!m_gfx) return;
    if(!wanted)
    {
        // Give the core's reservoirs back instead of keeping them (see
        // RTDIPass::EnsureResources); OnCompile re-Allocates them.
        if(!retain && m_main.core && m_main.core->IsAllocated())
        {
            const uint64_t bytes=m_main.core->ResidentBytes();
            m_main.core->Release(*m_gfx);
            m_status.residentBytes=0;
            LOG_INFO("[RESTIR_RELEASE] pass=%s backend=%s coreBytes=%llu",GetName(),m_status.effectiveName,static_cast<unsigned long long>(bytes));
        }
        // SHaRC: the cache goes with the core's set (a new one, cleared, when GI is
        // wanted again); the budget ladder's Raster stage retains both for its
        // recovery probe.
        if(!retain && m_sharc.IsAllocated())
        {
            const uint64_t bytes=m_sharc.ResidentBytes();
            m_sharc.Release(*m_gfx);
            m_sharc.RequestClear("allocate");
            LOG_INFO("[RTGI_SHARC] released bytes=%llu reason=not wanted",static_cast<unsigned long long>(bytes));
        }
        if(!retain && m_sharcActive) { m_sharcActive=false; m_sharcWhy="not wanted"; m_sharcLogPending=true; }
        return;
    }
    ApplyBackend();
    BuildPipelines();
    if(m_main.core) m_status.pipelineMs=m_main.core->PipelineMs();
    // RestirGIResult is on demand (Renderer::Compile); backed on the first
    // frame this pass is wanted with a pipeline, sticky from then on.
    if(m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines() && m_cb.IsValid()) m_graph.EnsurePhysicalTexture(m_result,*m_gfx);
    OnCompile(*m_gfx);
    ApplySharc();
    if(m_logBackend && (m_main.resourcesReady || m_pipelinesFailed))
    {
        if(m_pipelinesFailed) m_status.reason="pipeline build failed";
        LogBackend();
        m_logBackend=false;
    }
}
void RTGIPass::SetSettings(const Settings& in)
{
    if(!m_main.core) return;
    // Full resolution changes the trace size: the working set is rebuilt at
    // this service point (OnCompile resets the history). The spatial stage and
    // the candidate counts are constants of the next frame.
    m_fullResSetting=in.fullResolution; m_spatialSetting=in.spatial;
    m_denoiserSetting=in.denoiser;
    const uint32_t before=m_flags;
    ApplySettingFlags();
    if((before^m_flags)&4u) m_main.resourcesReady=false;
    // The denoiser picks NRD's kind and pools when the working set is built
    // (OnCompile): a change rebuilds it, and the history starts over.
    if((before^m_flags)&(1u|512u))
    {
        m_main.resourcesReady=false; ResetHistory();
        if(!m_shadow)   // the compare shadow follows the same settings
            LOG_INFO("[RTGI_SETTINGS] denoiser=%s nrd=%s",ReSTIR::DenoiserName(m_denoiserSetting),
                (m_flags&1u)?"none":((m_flags&512u)?"relax":"reblur"));
    }
    const uint32_t initial=std::clamp(in.initialCandidates,1u,16u);
    const uint32_t emitter=std::clamp(in.emitterCandidates,1u,64u);
    const uint32_t exact=std::clamp(in.hitExactLights,1u,64u);
    if(((before^m_flags)&(4u|16u)) || initial!=m_initialCandidates || emitter!=m_emitterCandidates || exact!=m_hitExactLights)
    {
        m_initialCandidates=initial; m_emitterCandidates=emitter; m_hitExactLights=exact;
        LOG_INFO("[RTGI_SETTINGS] fullResolution=%u spatial=%u initialCandidates=%u emitterCandidates=%u hitExactLights=%u",
            (m_flags&4u)?1u:0u,(m_flags&16u)?0u:1u,m_initialCandidates,m_emitterCandidates,m_hitExactLights);
    }
    RTGI::CoreSettings s;
    s.reuse=in.reuse; s.spatialRadius=in.spatialRadius; s.maxAge=in.maxAge; s.fullResolution=(m_flags&4u)!=0;
    s.spatialRadiusPinned=in.spatialRadiusPinned;
    m_coreSettings=s; m_haveCoreSettings=true;
    const float maxDistance=in.maxDistance; const uint32_t tier=in.tier;
    bool historyReset=false;
    const bool realloc=m_main.core->ApplySettings(s,historyReset);
    const bool wantReuse=m_main.core->Reuse();
    if(wantReuse!=((m_flags&8u)!=0)) m_flags=wantReuse?(m_flags|8u):(m_flags&~8u);
    if(realloc) m_main.resourcesReady=false;
    if(historyReset) ResetHistory();
    // raytracing.restirGiMaxDistance / restirGiTier (-gi_ray_tmax / -gi_tier
    // also set them; the row refuses NaN).
    const float d=std::clamp(maxDistance,1.f,1000.f);
    uint32_t t=std::min(tier,1u);
    // SHaRC: the settings' request (-gi_sharc / -gi_sharc_mode also set it). A
    // new content mode is a new cache: cleared, and a fresh history.
    m_sharcWanted=in.sharc; m_sharcPinned=in.sharcPinned;
    const uint32_t mode=std::min(in.sharcMode,1u);
    if(mode!=m_sharcMode)
    {
        m_sharcMode=mode;
        m_sharc.RequestClear("mode");
        ResetHistory();
    }
    // The cache holds Tier 1 shading (shadowed hit lights): Tier 0 leaks light
    // through walls and would be the fallback's expectation, so Tier 1 while
    // the cache is requested.
    const bool tierForced=m_sharcWanted && t==0;
    if(tierForced) t=1;
    if(tierForced!=m_tierForced) { m_tierForced=tierForced; m_sharcLogPending=true; }
    if(d!=m_rayTMax || t!=m_tier) ResetHistory();
    m_rayTMax=d; m_tier=t;
}
void RTGIPass::ReleaseWorkingSet(IGraphicsDevice& gfx,WorkingSet& ws)
{
    const bool main=&ws==&m_main;
    // NRD pools release immediately: the main view's drain first (the service point), a secondary
    // view's (its segment) retire.
    if(ws.nrd)
    {
        if(main) { gfx.WaitIdleAndReleaseDeferred(); ws.nrd.reset(); }
        else m_retiredNrd.emplace_back(std::move(ws.nrd),RHI::kFramesInFlight+1u);
    }
    for(auto& tex:ws.tex) { if(tex.IsValid()) gfx.DestroyTexture(tex); tex={}; }
    if(main)
        for(uint32_t i=0;i<RHI::kFramesInFlight;++i)
        {
            if(m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
            if(m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
            m_readback[i]={}; m_mapped[i]=nullptr; m_primed[i]=false;
        }
    if(ws.counter.IsValid()) gfx.DestroyBuffer(ws.counter);
    ws.counter={}; ws.resourcesReady=false;
    if(main)
    {
        if(m_rrDistance.IsValid()) gfx.DestroyTexture(m_rrDistance);
        m_rrDistance={};
    }
    if(ws.core) ws.core->Release(gfx);
    ws.w=ws.h=0; // the next build reallocates even at an unchanged size
    if(main) m_status.residentBytes=0;
}
void RTGIPass::OnCompile(IGraphicsDevice& gfx)
{
    // The main view's (the graph's compile and the service point); a secondary view's is made in its
    // segment (EnsureViewResources).
    if(m_ws!=&m_main) return;
    BuildWorkingSet(gfx,gfx.GetRenderWidth(),gfx.GetRenderHeight());
}

void RTGIPass::EnsureViewResources(uint32_t w,uint32_t h,bool wanted)
{
    if(m_ws==&m_main || !m_gfx || !m_hist) return;
    WorkingSet& ws=*m_ws;
    const bool can=wanted && m_wanted && m_pso.back().IsValid() && m_main.core && m_main.core->HasPipelines()
        && m_cb.IsValid();
    // Another core on the main view (a backend switch, a reload) or another flag word: this view's set
    // is made again.
    const uint32_t flags=ViewFlags();
    if(ws.core && (!can || ws.generation!=m_generation || ws.flags!=flags))
    {
        ReleaseWorkingSet(*m_gfx,ws);
        ws.core.reset();
    }
    if(!can) return;
    if(!ws.core)
    {
        ws.flags=flags; ws.generation=m_generation;
        ws.core=MakeCore(m_coreBackend);
        ws.core->AdoptPipelines(*m_main.core);
    }
    // The main view's settings, as SetSettings gives its core every frame, and its trace's distance and
    // tier (a change starts a new history, as the main view's does).
    if(m_haveCoreSettings)
    {
        bool reset=false;
        if(ws.core->ApplySettings(m_coreSettings,reset)) ws.resourcesReady=false;
        if(reset) m_hist->reset=m_hist->resetReservoir=true;
    }
    if(ws.rayTMax!=m_rayTMax || ws.tier!=m_tier) m_hist->reset=m_hist->resetReservoir=true;
    ws.rayTMax=m_rayTMax; ws.tier=m_tier;
    BuildWorkingSet(*m_gfx,w,h);
}

void RTGIPass::BuildWorkingSet(IGraphicsDevice& gfx,uint32_t w,uint32_t h)
{
    WorkingSet& ws=*m_ws;
    const bool main=m_ws==&m_main;
    if(!m_wanted || !m_pso.back().IsValid() || !ws.core || !ws.core->HasPipelines() || !m_cb.IsValid()) return;
    if(!w || !h) return;
    const uint32_t flags=BoundFlags();
    if(w==ws.w && h==ws.h && ws.resourcesReady && (!ws.placedMask || !m_aliasArena || ws.placedEpoch==m_aliasArena->Epoch()))
    {
        // The shell's set is current; re-Allocate a released core (new history).
        if(!ws.core->IsAllocated())
        {
            if(!ws.core->Allocate(gfx,ws.tw,ws.th)) { ws.resourcesReady=false; return; }
            m_hist->reset=m_hist->resetReservoir=true;
            if(main) m_status.residentBytes=ws.core->ResidentBytes();
        }
        return;
    }
    ReleaseWorkingSet(gfx,ws); ws.w=w; ws.h=h;
    const uint32_t scale=(flags&4u)?1:2; ws.tw=(w+scale-1)/scale; ws.th=(h+scale-1)/scale;
    const auto& formats=ReSTIRTransient::giFormats;
    static_assert(ReSTIRTransient::giFormats.size()==TextureCount);
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
        auto td=ReSTIRTransient::Describe(ws.tw,ws.th,formats[i]);
        ws.states[i]=td.layout;
        if(placed && gfx.CreateTexturePlaced(td,*m_aliasArena,layout.offsets[i],ws.tex[i])) ws.placedMask|=1u<<i;
        else if(!gfx.CreateTexture(td,ws.tex[i])) { LOG_ERROR("RTGIPass: texture %zu failed",i); return; }
    }
    ws.placedEpoch=(placed && m_aliasArena)?m_aliasArena->Epoch():0;
    if(main) LOG_INFO("[RESTIR_ARENA] pass=%s width=%u height=%u placedMask=%u bytes=%llu epoch=%u",
        GetName(),ws.tw,ws.th,ws.placedMask,static_cast<unsigned long long>(layout.bytes),ws.placedEpoch);
    RHI::GPUBufferDesc bd{}; bd.size=kCounterCount*sizeof(uint32_t); bd.stride=sizeof(uint32_t);
    bd.usage=RHI::Usage::DEFAULT; bd.bindFlags=RHI::BindFlag::UNORDERED_ACCESS;
    bd.miscFlags=RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    if(!gfx.CreateBuffer(bd,ws.counter)) { LOG_ERROR("RTGIPass: counters failed"); return; }
    ws.counterState=RHI::ResourceState::UNDEFINED;
    bd.usage=RHI::Usage::READBACK; bd.bindFlags=RHI::BindFlag::NONE;
    if(main)   // the [RTGI] lines are the main view's
        for(uint32_t i=0;i<RHI::kFramesInFlight;++i)
            if(!gfx.CreateBuffer(bd,m_readback[i]) || !(m_mapped[i]=static_cast<const uint32_t*>(gfx.MapBuffer(m_readback[i]))))
            { LOG_ERROR("RTGIPass: readback failed"); return; }
    // The core's working set after the shell's.
    if(!ws.core->Allocate(gfx,ws.tw,ws.th)) return;
    if(!(flags&1u))
    {
        ws.nrd=std::make_unique<NrdDenoiser>(); ws.nrd->SetKind((flags&512u)?NrdDenoiser::Kind::RelaxDiffuse:NrdDenoiser::Kind::ReblurDiffuse);
        // FP32 illumination pools (default), as DI's: the typed FP16 UAV store rounds
        // toward zero, and RELAX's temporal loop stores one freshly truncated value a
        // frame, so the loss grows with the history.
        ws.nrd->SetGiFloat32Pool(true);
        if(!ws.nrd->Resize(gfx,ws.tw,ws.th)) { LOG_ERROR("RTGIPass: NRD initialization failed"); return; }
    }
    if(main && m_rrRaw)   // ray reconstruction is the main view's
    {
        // The DLSS-RR diffuse hit distance, render-sized (Resolve writes every render
        // pixel, at either trace resolution).
        RHI::TextureDesc td{}; td.width=w; td.height=h; td.format=RHI::Format::R16_FLOAT;
        td.bindFlags=RHI::BindFlag::UNORDERED_ACCESS|RHI::BindFlag::SHADER_RESOURCE;
        td.usage=RHI::Usage::DEFAULT; td.layout=RHI::ResourceState::UNORDERED_ACCESS; td.debugName="RTGI.RRDiffuseDistance";
        if(!gfx.CreateTexture(td,m_rrDistance)) LOG_ERROR("RTGIPass: RR diffuse distance allocation failed");
        m_rrDistanceState=td.layout;
    }
    m_hist->reset=m_hist->resetReservoir=true; ws.resourcesReady=true;
    if(main) m_status.residentBytes=ws.core->ResidentBytes();
}
void RTGIPass::Shutdown(IGraphicsDevice& gfx)
{
    ReleaseWorkingSet(gfx,m_main); m_cb.Destroy(gfx); gfx.DestroyPipelineLayout(m_layout); for(auto& p:m_pso) gfx.DestroyPipelineState(p);
    // SHaRC: the cache and its kernels too (a hot reload re-allocates it at the
    // next service point, cleared).
    ReleaseSharcPipelines();
    if(m_sharc.IsAllocated()) { m_sharc.Release(gfx); m_sharc.RequestClear("allocate"); }
    m_sharcActive=false; m_sharcLogPending=true;
    if(m_main.core) m_main.core->ReleasePipelines(gfx);
    m_main.core.reset();
    m_main.active=false;
    m_retiredNrd.clear();   // after the GPU drain
    ++m_generation;         // the views' cores recorded with the pipelines just released
}
void RTGIPass::ReloadShaders(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred(); Shutdown(gfx); Init(gfx);
    if(m_wanted) { ApplyBackend(); BuildPipelines(); }   // in use: rebuilt now, not a frame late
    OnCompile(gfx);
}
const char* RTGIPass::GetInertReason() const
{
    const WorkingSet& ws=*m_ws;   // the bound view's
    if(!m_pso.back().IsValid() || !m_cb.IsValid() || !ws.core) return "pipeline";
    if(const char* why=ws.core->InertReason()) return why;
    if(!ws.resourcesReady) return "resources";
    if(!(BoundFlags()&1u) && (!ws.nrd || !ws.nrd->IsReady())) return "NRD";
    const auto* result=m_graph.GetPhysicalTexture(m_result);
    return result && result->IsValid()?nullptr:"result";
}
uint64_t RTGIPass::GetUpcomingOutputSrv() const
{ return GetInertReason()?0:m_gfx->GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(m_result)); }
void RTGIPass::SetFrameData(const FrameData& fd,bool active,uint32_t frame,float jitterX,float jitterY)
{
    // Reservoirs store a world-space secondary point whose radiance is re-traced
    // and re-shaded every frame before reuse, so material/texture/surface-cache
    // revisions and light changes need no reset. Those fingerprints also move
    // every frame with skinned TLAS records, animated lights or shadow-slot
    // reassignment: resetting on them would clear the denoiser every frame.
    WorkingSet& ws=*m_ws;   // the bound view's
    if(active!=ws.active) ResetHistory();
    // A change of the light SET (a light added or removed, even with the count
    // unchanged) is not covered by the per-frame re-shade: NRD's history still
    // holds the old irradiance and fades it over giHistorySeconds. Drop the
    // denoiser history (the reservoirs re-shade their stored points against the
    // new list), the same rule RTDIPass uses.
    else if(active && (fd.lightCount!=ws.fd.lightCount || !fd.lightsIdentity)) m_hist->reset=true;
    ws.fd=fd; ws.active=active; ws.frame=frame; ws.jitterX=jitterX; ws.jitterY=jitterY;
}
RHI::CommandList RTGIPass::Execute(RHI::CommandList cl)
{
    WorkingSet& ws=*m_ws;   // the bound view's
    if(!ws.active || GetInertReason() || !ws.fd.tlasVA || !ws.fd.instanceVA || !ws.fd.buffers) return cl;
    IGraphicsDevice& gfx=cl.GetDevice();
    const bool main=m_ws==&m_main;
    const uint32_t flags=BoundFlags();
    const bool rrRaw=main && m_rrRaw;   // ray reconstruction is the main view's
    for(size_t i=0;i<ws.tex.size();++i)
        if(ws.placedMask&(1u<<i)) gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr,&ws.tex[i]),cl);

    const auto slot=gfx.GetFrameIndex();
    if(main && m_primed[slot])   // the [RTGI] lines are the main view's
    {
        const auto* s=m_mapped[slot];
        if(Sys::CommandLine::Get().Has("-restir_history_probe"))
            LOG_INFO("[RESTIR_HISTORY] pass=%s frame=%u reset=%u reusedTemporal=%u",
                GetName(),m_readbackFrame[slot],m_readbackReset[slot],s[6]);
        if(ws.frame%30==0) LOG_INFO("%s eligible=%u rays=%u resolved=%u visibility=%u skinnedHits=%u nonfinite=%u half=%u maxDistance=%.3f reusedTemporal=%u reusedSpatial=%u validated=%u rejected=%u spatialVisibility=%u reuse=%u feedback=%u radius=%u luminanceResets=%u manyLightHits=%u boiled=%u emitters=%u emitterVisible=%u emitterSelected=%u backend=%s",
            m_shadow?"[RTGI_SHADOW]":"[RTGI]",
            s[0],s[1],s[2],s[3],s[4],s[5],(flags&4u)?0u:1u,m_rayTMax,s[6],s[7],s[8],s[9],s[10],(flags&8u)?1u:0u,ws.core->Feedback()?1u:0u,ws.core->SpatialRadius(),s[11],s[12],s[13],
            (flags&65536u)?ws.fd.emitterCount:0u,s[14],s[15],m_status.effectiveName);
        if(ws.frame%30==0) LOG_INFO("%s belowHorizon=%u backfaceMiss=%u noCompatible=%u jacobianLeash=%u ageCap=%u occluded=%u vanishedMiss=%u vanishedDistance=%u vanishedOther=%u",
            m_shadow?"[RTGI_SHADOW_REUSE]":"[RTGI_REUSE]",s[16],s[17],s[18],s[19],s[20],s[21],s[22],s[23],s[24]);
        // SHaRC (counter slots 25-31, ReSTIRCounters.h).
        if(ws.frame%30==0 && m_sharcActive)
            LOG_INFO("%s frame=%u cacheHits=%u cacheMisses=%u gated=%u ineligible=%u updatePaths=%u badRadiance=%u modeFallbacks=%u luminanceResets=%u mode=%s clears=%u",
                m_shadow?"[RTGI_SHADOW_SHARC]":"[RTGI_SHARC]",ws.frame,s[25],s[26],s[27],s[28],s[29],s[30],s[31],s[11],
                m_sharcMode==1?"multi":"parity",SharcSourceCache()->Clears());
        if(s[5]) LOG_ERROR("RTGIPass%s: nonfinite pixels=%u",m_shadow?" (shadow)":"",s[5]);
    }
    auto index=[&](uint64_t h){ return h?gfx.DescriptorHeapIndexFromGpuHandle(h):0u; };
    auto graphIndex=[&](RG::RGTextureHandle h){ return index(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(h))); };
    Constants cb{}; using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),XMMatrixTranspose(XMMatrixInverse(nullptr,XMLoadFloat4x4(&ws.fd.viewProj))));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.worldToView),XMMatrixTranspose(XMLoadFloat4x4(&ws.fd.view)));
    std::memcpy(cb.camera,&ws.fd.camera,12); std::memcpy(cb.sunDirection,&ws.fd.sunDirection,12); std::memcpy(cb.sunColor,&ws.fd.sunColor,12);
    cb.rayTMax=m_rayTMax; cb.frame=ws.frame; cb.seed=m_seed; cb.flags=flags; cb.constantRadiance=m_constantRadiance;
    cb.width=ws.w; cb.height=ws.h; cb.traceW=ws.tw; cb.traceH=ws.th;
    cb.depthIdx=graphIndex(m_depth); cb.normalIdx=graphIndex(m_normal); cb.albedoIdx=graphIndex(m_albedo); cb.surfaceIdx=graphIndex(m_surface);
    cb.velocityIdx=graphIndex(m_velocity); cb.materialsIdx=ws.fd.materialsIdx; cb.lightsIdx=ws.fd.lightsIdx; cb.lightCount=ws.fd.lightCount;
    cb.alphaTestEnabled=ws.fd.alphaVA!=0; cb.alphaInstanceCount=ws.fd.instanceCount;
    cb.counterUav=index(gfx.GetBufferUAVGpuHandle(ws.counter));
    cb.diffUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[DiffIn])); cb.normalUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[Normal]));
    cb.viewZUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[ViewZ])); cb.mvUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[MV]));
    cb.diffSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[ws.nrd?DiffOut:DiffIn]));
    cb.coverageUav=index(gfx.GetTextureUAVGpuHandle(ws.tex[Coverage])); cb.coverageSrv=index(gfx.GetTextureSRVGpuHandle(ws.tex[Coverage]));
    cb.volumeIdx=index(ws.fd.volumeSrv); cb.irradianceIdx=index(ws.fd.irradianceTable); cb.ddgiDepthIdx=index(ws.fd.depthTable);
    cb.probeIdx=index(ws.fd.probeTable); cb.volumeCount=ws.fd.volumeCount; cb.worldGrid=ws.fd.worldGrid; cb.tier=m_tier;
    cb.rrDistanceUav=rrRaw && m_rrDistance.IsValid()?index(gfx.GetTextureUAVGpuHandle(m_rrDistance)):0u;
    const bool emitters=(flags&65536u) && ws.fd.emitterCount && ws.fd.emitterSrv && ws.fd.emitterTableUav;
    cb.emitterIdx=emitters?index(ws.fd.emitterSrv):0; cb.emitterTableIdx=emitters?index(ws.fd.emitterTableUav):0;
    cb.emitterCount=emitters && cb.emitterIdx && cb.emitterTableIdx?ws.fd.emitterCount:0; cb.emitterCandidates=m_emitterCandidates;
    cb.initialCandidates=m_initialCandidates; cb.hitExactLights=m_hitExactLights; cb.hitLightSamples=m_hitLightSamples;
    cb.localEmitterLimit=m_localEmitterLimit;
    // The core's fields: budget bits, reprojection, reservoir indices, caps.
    RTGI::FillContext fill;
    fill.shellFlags=flags; fill.frame=ws.frame; fill.resetReservoir=m_hist->resetReservoir;
    fill.jitterX=ws.jitterX; fill.jitterY=ws.jitterY; fill.budgetStage=m_budgetStage;
    ws.core->FillConstants(gfx,cb,fill);
    // SHaRC: the RTGI_SHARC kernels' tail of the constants.
    const bool sharc=main && SharcRuns();   // the cache is the main view's (world state)
    if(sharc)
    {
        const SharcCache::Indices cache=SharcSourceCache()->GetIndices(gfx);
        cb.sharcHashUav=cache.hashUav; cb.sharcAccumulationUav=cache.accumulationUav;
        cb.sharcResolvedUav=cache.resolvedUav; cb.sharcCapacity=cache.capacity;
        cb.sharcSceneScale=m_sharcConfig.sceneScale; cb.sharcRadianceScale=m_sharcConfig.radianceScale;
        cb.sharcMaxVoxel=m_sharcMaxVoxel;
        // The bound arm (-gi_sharc_bound) shades no hit and updates nothing; the
        // shadow pass queries what the main pass updated this frame.
        cb.sharcFlags=(m_sharcBound?RTGI::kSharcBound:RTGI::kSharcQuery)|(m_sharcMode==1?RTGI::kSharcMulti:0u)
                     |(m_sharcForceMiss?RTGI::kSharcForceMiss:0u)|(!m_shadow && !m_sharcBound?RTGI::kSharcUpdate:0u);
        const float* prev=m_hist->sharcHasPrev?m_hist->sharcPrevCamera:cb.camera;
        cb.sharcPrevCamera[0]=prev[0]; cb.sharcPrevCamera[1]=prev[1]; cb.sharcPrevCamera[2]=prev[2];
        cb.sharcFrame=ws.frame+(m_seed-1u)*0x9E3779B9u;
        cb.sharcUpdateTile=m_sharcConfig.updateTile; cb.sharcUpdateBounces=m_sharcConfig.updateBounces;
        cb.sharcAccumulationFrames=m_sharcConfig.accumulationFrames; cb.sharcStaleFrames=m_sharcConfig.staleFrames;
        cb.skyIdx=ws.fd.skyIdx; cb.environmentScale=ws.fd.environmentScale;
        // uint(value x scale) is added once per path reaching an entry in a
        // frame: 256 deposits of this much fill the 32-bit accumulator.
        cb.sharcDepositMax=4294967296.f/(m_sharcConfig.radianceScale*256.f);
        std::memcpy(m_hist->sharcPrevCamera,cb.camera,sizeof(m_hist->sharcPrevCamera)); m_hist->sharcHasPrev=true;
    }
    *m_cb.Current(gfx)=cb;
    static const bool s_cbDigest=Sys::CommandLine::Get().Has("-restir_cb_digest");
    // Without the cache the digest covers exactly the bytes the kernels without
    // SHaRC declare (RTGI::kConstantsOffBytes), so it stays comparable across builds.
    if(s_cbDigest)
        LOG_INFO("[RESTIR_CB] pass=%s frame=%u hash=%016llx flags=%08x",GetName(),ws.frame,
            static_cast<unsigned long long>(Sys::WordHash64(&cb,sharc?sizeof(cb):RTGI::kConstantsOffBytes)),cb.flags);
    auto bind=[&] {
        namespace S=ReSTIR::ShellSlots;
        // The shell layout, through the first entry's pipeline: every shell,
        // core and SHaRC pipeline is built on it, so the arguments hold across
        // their binds.
        gfx.BindComputePipelineState(m_pso[0],cl); gfx.SetComputeRootCBV(S::CB,m_cb.CurrentBuffer(gfx),0,cl);
        gfx.SetComputeRootSRVAddress(S::TLAS, ws.fd.tlasVA, cl); gfx.SetComputeRootSRVAddress(S::Instances, ws.fd.instanceVA, cl);
        gfx.SetComputeRootSRVAddress(S::Alpha, ws.fd.alphaVA, cl); gfx.SetComputeDescriptorTable(S::Buffers,ws.fd.buffers,cl);
        gfx.SetComputeDescriptorTable(S::Textures,gfx.GetBindlessTextureTableGpuHandle(),cl);
        gfx.SetComputeDescriptorTable(S::Result,gfx.GetTextureUAVGpuHandle(*m_graph.GetPhysicalTexture(m_result)),cl);
    };
    auto state=[&](size_t i,RHI::ResourceState after) {
        if(ws.states[i]!=after) gfx.PushBarrier(RHI::GPUBarrier::Image(&ws.tex[i],ws.states[i],after),cl); ws.states[i]=after;
    };
    auto barrier=[&] { gfx.PushBarrier(RHI::GPUBarrier::Memory(),cl); };
    auto dispatch=[&](size_t p,uint32_t x,uint32_t y,const char* name) {
        const auto timer=gfx.BeginGPUTimestamp(cl,name); gfx.BindComputePipelineState(m_pso[p],cl); gfx.DispatchCompute(x,y,1,cl); gfx.EndGPUTimestamp(cl,timer);
    };
    for(size_t i=0;i<ws.tex.size();++i) state(i,RHI::ResourceState::UNORDERED_ACCESS);
    if(ws.counterState!=RHI::ResourceState::UNORDERED_ACCESS) gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.counter,ws.counterState,RHI::ResourceState::UNORDERED_ACCESS),cl);
    ws.counterState=RHI::ResourceState::UNORDERED_ACCESS;
    RTGI::CoreFrame frame;
    frame.gfx=&gfx; frame.cl=cl; frame.traceW=ws.tw; frame.traceH=ws.th;
    frame.jitterX=ws.jitterX; frame.jitterY=ws.jitterY; frame.regions=m_regions;
    frame.sharc=sharc;
    ws.core->PreShell(frame);
    bind(); dispatch(0,1,1,m_regions->clear); barrier();
    // SHaRC, the main pass only, after the counter Clear (the update counts):
    // clear (a new cache, world, mode) -> this frame's sparse update -> resolve,
    // which Trace and the validation then query (SHaRC's Integration.md order).
    if(sharc && !m_shadow && !m_sharcBound)
    {
        auto run=[&](SharcPso p,uint32_t x,uint32_t y,const char* name) {
            const auto timer=gfx.BeginGPUTimestamp(cl,name); gfx.BindComputePipelineState(m_sharcPso[p],cl);
            gfx.DispatchCompute(x,y,1,cl); gfx.EndGPUTimestamp(cl,timer); barrier();
        };
        m_sharc.Transition(gfx,cl);
        const uint32_t groups=(m_sharc.Capacity()+255)/256;   // <= 2^23 entries: <= 32768 groups
        if(const char* why=m_sharc.TakeClear())
        {
            run(SharcClear,groups,1,m_regions->sharcClear);
            LOG_INFO("[RTGI_SHARC] cleared=%u reason=%s capacity=%u",m_sharc.Clears(),why,m_sharc.Capacity());
        }
        const uint32_t tile=std::max(m_sharcConfig.updateTile,1u);
        const uint32_t tilesX=(ws.w+tile-1)/tile,tilesY=(ws.h+tile-1)/tile;
        run(SharcUpdate,(tilesX+7)/8,(tilesY+7)/8,m_regions->sharcUpdate);
        run(SharcResolve,groups,1,m_regions->sharcResolve);
    }
    if(cb.emitterCount)
    {
        // Per-emitter world area and power (one group per emitter), then the
        // power CDF (one group). The table decays to COMMON between frames
        // and is promoted to UAV implicitly, like any buffer.
        const uint32_t x=std::min(cb.emitterCount,65535u),y=(cb.emitterCount+65534u)/65535u;
        dispatch(3,x,y,m_regions->emitterArea); barrier();
        dispatch(4,1,1,m_regions->emitterCdf); barrier();
    }
    if(sharc)
    {
        // Trace's RTGI_SHARC build: its hit shading queries the cache.
        const auto timer=gfx.BeginGPUTimestamp(cl,m_regions->trace); gfx.BindComputePipelineState(m_traceSharc,cl);
        gfx.DispatchCompute((ws.tw+7)/8,(ws.th+7)/8,1,cl); gfx.EndGPUTimestamp(cl,timer); barrier();
    }
    else { dispatch(1,(ws.tw+7)/8,(ws.th+7)/8,m_regions->trace); barrier(); }
    ws.core->Record(frame);
    state(DiffIn,RHI::ResourceState::SHADER_RESOURCE_COMPUTE); state(Normal,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    state(ViewZ,RHI::ResourceState::SHADER_RESOURCE_COMPUTE); state(Coverage,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    if(ws.nrd)
    {
        NrdDenoiser::Params np{}; std::memcpy(np.worldToView,&ws.fd.view,64); std::memcpy(np.viewToClip,&ws.fd.projection,64);
        np.width=ws.tw; np.height=ws.th; np.frameIndex=ws.frame; np.resetHistory=m_hist->reset; np.diffHitDistScaleA=m_rayTMax;
        np.denoisingRange=ws.fd.farZ*0.99f;
        // RELAX's settings: pass.rtgi.nrd*, read every frame.
        np.giPrepassRadius=std::clamp(::Settings::Pass::rtgiNrdPrepass(),0.f,60.f);
        np.giAntiFirefly=::Settings::Pass::rtgiNrdAntiFirefly();
        np.giVarianceHistory=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiNrdVarianceHistory(),0,16));
        np.giAntilagReset=std::clamp(::Settings::Pass::rtgiNrdAntilagReset(),0.f,1.f);
        np.giAntilagSpatialSigma=std::clamp(::Settings::Pass::rtgiNrdAntilagSigma(),.5f,20.f);
        np.giAtrous=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiNrdAtrous(),2,8));
        np.giHistorySeconds=std::clamp(::Settings::Pass::rtgiNrdHistorySeconds(),.05f,2.f);
        np.giMinHistoryFrames=static_cast<uint32_t>(std::clamp(::Settings::Pass::rtgiNrdMinHistory(),1,60));
        if(ws.frame%120==0)
            LOG_INFO("%s frame=%u fp32=%u",m_shadow?"[RTGI_SHADOW_FILTER]":"[RTGI_FILTER]",ws.frame,ws.nrd->GiFloat32Pool()?1u:0u);
        const float scale=(flags&4u)?1.f:.5f; np.jitter[0]=ws.jitterX*scale; np.jitter[1]=ws.jitterY*scale;
        auto binding=[&](nrd::ResourceType type,size_t i,RHI::ResourceState s) {
            return NrdDenoiser::Binding{type,{&ws.tex[i],s}};
        };
        const auto srv=RHI::ResourceState::SHADER_RESOURCE_COMPUTE,uav=RHI::ResourceState::UNORDERED_ACCESS;
        NrdDenoiser::Binding bindings[]={binding(nrd::ResourceType::IN_MV,MV,uav),
            binding(nrd::ResourceType::IN_NORMAL_ROUGHNESS,Normal,srv),
            binding(nrd::ResourceType::IN_VIEWZ,ViewZ,srv),
            binding(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,DiffIn,srv),
            binding(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST,DiffOut,uav)};
        // Named after the denoiser that actually runs (RELAX by default).
        const auto timer=gfx.BeginGPUTimestamp(cl,(flags&512u)?m_regions->relax:m_regions->reblur);
        if(!ws.nrd->Denoise(gfx,cl,np,bindings,5)) LOG_ERROR("RTGIPass: NRD dispatch failed");
        gfx.EndGPUTimestamp(cl,timer); gfx.BindDescriptorHeaps(cl); bind(); state(DiffOut,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    }
    if(cb.rrDistanceUav && m_rrDistanceState!=RHI::ResourceState::UNORDERED_ACCESS)
    { gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrDistance,m_rrDistanceState,RHI::ResourceState::UNORDERED_ACCESS),cl); m_rrDistanceState=RHI::ResourceState::UNORDERED_ACCESS; }
    dispatch(2,(ws.w+7)/8,(ws.h+7)/8,m_regions->resolve);
    if(cb.rrDistanceUav)
    { gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrDistance,m_rrDistanceState,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl); m_rrDistanceState=RHI::ResourceState::SHADER_RESOURCE_COMPUTE; }
    if(main)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.counter,ws.counterState,RHI::ResourceState::COPY_SRC),cl);
        ws.counterState=RHI::ResourceState::COPY_SRC; gfx.CopyBuffer(ws.counter,m_readback[slot],kCounterCount*sizeof(uint32_t),cl); m_primed[slot]=true;
        m_readbackFrame[slot]=ws.frame; m_readbackReset[slot]=m_hist->resetReservoir;
    }
    m_hist->reset=m_hist->resetReservoir=false; return cl;
}

void RTGIPass::OnService(RG::ServiceContext& c)
{
    // The static BLAS set came complete - the main pass only.
    if(c.stage==RG::ServiceStage::StaticGeometry){ OnStaticGeometryComplete(); return; }
    for(size_t i=0;i<m_retiredNrd.size();)
    {
        if(--m_retiredNrd[i].second==0) m_retiredNrd.erase(m_retiredNrd.begin()+static_cast<ptrdiff_t>(i));
        else ++i;
    }
    const ServiceRequest& r=m_request;
    if(!m_shadow)
    {
        SetBackend(r.backend);
        SetRayReconstruction(r.raw);
        SetSettings(r.settings);
        EnsureResources(r.wanted,r.retain);
        c.board.giEngineRuns=GetCoreBackend()==ReSTIR::Backend::Engine;
        c.board.giHistoryReset=r.wanted && HistoryResetPending();
    }
    else
    {
        // Compare mode: the shadow instance runs the other core.
        SetBackend(c.board.giEngineRuns ? ReSTIR::Backend::Rtxdi : ReSTIR::Backend::Engine);
        SetSettings(r.settings);
        EnsureResources(r.wanted,false);
    }
}
