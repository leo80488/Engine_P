#include "RenderGraph/RenderPass/PathTracePass.h"
#include "Settings/PassSettings.h"     // pass.pathTrace.*
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/ShaderDiskCache.h"
#include "RenderGraph/PassTextureNames.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/WordHash.h"
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>

namespace
{
// The path tracer's pipeline layout (Init): b0, the TLAS / instance / alpha-record root SRVs
// (t0..t2), the bindless buffer and texture tables, three UAV tables (u0 the result, u1 the
// accumulation, u2 the statistics) and the linear-wrap sampler s1.
constexpr RHI::Slots::CmpParam kRootCB       { 0 };
constexpr RHI::Slots::CmpParam kRootTLAS     { 1 };
constexpr RHI::Slots::CmpParam kRootInstances{ 2 };
constexpr RHI::Slots::CmpParam kRootAlpha    { 3 };
constexpr RHI::Slots::CmpParam kRootBuffers  { 4 };
constexpr RHI::Slots::CmpParam kRootTextures { 5 };
constexpr RHI::Slots::CmpParam kRootResult   { 6 };
constexpr RHI::Slots::CmpParam kRootAccum    { 7 };
constexpr RHI::Slots::CmpParam kRootStats    { 8 };

// -bsdf_probe's dielectric half (BSDFProbeDielectric.cs.hlsl, the same constants): per case
// 256 counts, 256 pdfs, 256 f values and 8 sums, then 5 partial sums per group, all in the
// statistics buffer past every other probe's words.
constexpr uint32_t kDielectricProbeBase      = 4096;
constexpr uint32_t kDielectricProbeCases     = 9;
constexpr uint32_t kDielectricProbeBins      = 256;
constexpr uint32_t kDielectricProbeCaseWords = 3 * kDielectricProbeBins + 8;
constexpr uint32_t kDielectricProbeGroups    = 64;
constexpr uint32_t kDielectricProbeEnd       = kDielectricProbeBase
    + kDielectricProbeCases * kDielectricProbeCaseWords + kDielectricProbeCases * kDielectricProbeGroups * 5;

// -bsdf_probe's participating-media half (MediaProbe.cs.hlsl, the same constants), past the
// dielectric words: Henyey-Greenstein direction cases (counts, then densities at the bin
// centres), free-flight cases (64 counts and their side words) and equi-angular cases (counts,
// then densities), then the free-flight groups' ratio-tracking partial sums.
constexpr uint32_t kMediaProbeBase       = 14336;
constexpr uint32_t kMediaProbeHgCases    = 5;
constexpr uint32_t kMediaProbeHgBins     = 256;
constexpr uint32_t kMediaProbeFfBase     = kMediaProbeBase + kMediaProbeHgCases * 2 * kMediaProbeHgBins;
constexpr uint32_t kMediaProbeFfCases    = 8;
constexpr uint32_t kMediaProbeFfBins     = 64;
constexpr uint32_t kMediaProbeFfWords    = 80;
constexpr uint32_t kMediaProbeEaBase     = kMediaProbeFfBase + kMediaProbeFfCases * kMediaProbeFfWords;
constexpr uint32_t kMediaProbeEaCases    = 2;
constexpr uint32_t kMediaProbeEaBins     = 64;
constexpr uint32_t kMediaProbeGroups     = 64;
constexpr uint32_t kMediaProbeEnd        = kMediaProbeEaBase + kMediaProbeEaCases * 2 * kMediaProbeEaBins
    + kMediaProbeFfCases * kMediaProbeGroups * 2;
static_assert(kMediaProbeBase >= kDielectricProbeEnd, "the media probe overlaps the dielectric probe");

// Logs n probe words from `words`, 32 a line (a log message holds 1023 characters): counts as
// integers, everything else as floats.
void LogProbeWords(const char* tag, uint32_t probeCase, const char* kind, const uint32_t* words,
                   uint32_t n, bool counts)
{
    for (uint32_t first = 0; first < n; first += 32)
    {
        std::string values;
        char number[32];
        for (uint32_t i = first; i < first + 32 && i < n; ++i)
        {
            float asFloat;
            std::memcpy(&asFloat, &words[i], sizeof(asFloat));
            if (counts) std::snprintf(number, sizeof(number), "%u", words[i]);
            else        std::snprintf(number, sizeof(number), "%.9g", asFloat);
            if (i != first) values += ',';
            values += number;
        }
        LOG_INFO("[%s] case=%u kind=%s first=%u values=%s", tag, probeCase, kind, first, values.c_str());
    }
}

// -pt_stability_log: why the image restarts ([PT_STABILITY], [PT_MOTION]).
bool StabilityLog()
{
    static const bool s_on = Sys::CommandLine::Get().Has("-pt_stability_log");
    return s_on;
}
}

PathTracePass::PathTracePass(RG::RenderGraph& graph, RG::RGTextureHandle albedo,
    RG::RGTextureHandle normal, RG::RGTextureHandle surface,
    RG::RGTextureHandle depth, RG::RGTextureHandle result)
    : m_graph(graph), m_albedo(albedo), m_normal(normal), m_surface(surface), m_depth(depth), m_result(result) {}

void PathTracePass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_albedo); b.ReadSRV(m_normal); b.ReadSRV(m_surface); b.ReadSRV(m_depth);
    // The rtxdi mode's temporal reuse and primary direct light.
    if (m_velocity.IsValid()) b.ReadSRV(m_velocity);
    if (m_diResult.IsValid()) b.ReadSRV(m_diResult);
    // The front-layer reflection's stamp (TranslucentFrontLayerPass, declared earlier); the
    // velocity feeds its denoiser.
    if (m_frontDepth.IsValid())
    {
        b.ReadSRV(m_frontDepth); b.ReadSRV(m_frontNormal); b.ReadSRV(m_frontSurface);
        if (m_frontVelocity.IsValid()) b.ReadSRV(m_frontVelocity);
    }
    b.WriteUAV(m_result);
    // The sky the misses sample (FrameData::skyIdx - SkyIBL's specular cube while the atmosphere
    // draws the sky; SkyIBLPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kSkySpecularCube });
}

void PathTracePass::Init(IGraphicsDevice& gfx)
{
    ENGINE_CHECK(m_hist != nullptr, "PathTracePass: no view bound before Init (PassSet::SetView before its registration)");
    m_gfx = &gfx;
    const auto& cmd = Sys::CommandLine::Get();
    m_seed = static_cast<uint32_t>(cmd.GetInt("-seed",1));
    m_uniformLights = cmd.Has("-pt_uniform_lights");
    m_lastNeeOff = cmd.Has("-pt_last_nee_off");
    // A reference with the ReSTIR GI shell's model at every bounce vertex: bounce rays pass
    // through alpha-blend instances and keep back faces, a single-sided back face ends the path,
    // and under -pt_diffuse a bounce vertex reflects albedo*(1-metal).
    m_giShellModel = cmd.Has("-pt_gi_shell_model");
    if (m_giShellModel)
        LOG_INFO("[PT_GI_SHELL_MODEL] enabled=1 diffuse=%u", cmd.Has("-pt_diffuse") ? 1u : 0u);
    // -pt_front_off: glass keeps the forward fold's screen tiers on this path
    // (the A/B of the FrontReflection entry, SetFrontLayer).
    m_frontOff = cmd.Has("-pt_front_off");
    // -pt_npr_raster: toon receivers keep the raster lighting inputs (DDGI /
    // sky taps, cascades) on this path (the A/B of SetNprLighting).
    m_nprOff = cmd.Has("-pt_npr_raster");
    m_flags = (cmd.Has("-pt_diffuse") ? 1u : 0u) | (cmd.Has("-pt_environment") ? 2u : 0u) | (cmd.Has("-pt_no_sun_nee") ? 4u : 0u) | (cmd.Has("-pt_raycounts") ? 8u : 0u);
    m_constantEnvironment = std::max(0.f, cmd.GetFloat("-pt_environment", 1.f));
    m_sunAngularRadius = std::clamp(Settings::Pass::ptSunRadius(),0.001f,1.f);
    if (cmd.Has("-bsdf_probe")) m_flags |= 24u;
    if (cmd.Has("-pt_nrd_reference")) m_flags |= 32u;
    if (cmd.Has("-pt_direct_only")) m_flags |= 64u;
    if (cmd.Has("-pt_single_sample")) m_flags |= 4096u;
    // Reference arm for receiver self-hits: continue along the geometric
    // normal and cut directions below it (needs -pt_camera_primary for the
    // primary vertex; later vertices always have it).
    if (cmd.Has("-pt_geo_horizon")) m_flags |= 256u;
    // Area does not force the ray counters (a clear dispatch, an atomic per counted ray, a
    // readback and 4-5 log lines a frame); -pt_raycounts turns them on, as for every mode. The
    // diagnostics below that add flag 8 force them: their evidence is the counters.
    if (cmd.Has("-pt_area_restir")) m_flags |= 16384u;
    if (cmd.Has("-pt_area_spatial")) m_flags |= 32768u | 16384u;
    if(m_areaModeRequested>0) m_flags |= 16384u;
    if(m_areaModeRequested==2) m_flags |= 32768u;
    if(cmd.Has("-pt_area_fractional")) m_flags |= 65536u;
    if(cmd.Has("-pt_area_overlap")) m_flags |= 262144u | 65536u;
    if(cmd.Has("-pt_area_overlap_holes")) m_flags |= 524288u | 262144u | 65536u;
    if(cmd.Has("-pt_area_overlap_no_escape")) m_flags |= 1048576u; // Diagnostic negative control only.
    if(cmd.Has("-pt_area_primary_context") || cmd.Has("-pt_area_context_corrupt")) m_flags |= 4194304u | 16384u | 8u;
    if(cmd.Has("-pt_area_context_corrupt")) m_flags |= 8388608u; // Diagnostic negative control.
    if(cmd.Has("-pt_lens_cone_probe")) m_flags |= 16777216u | 8u;
    if(cmd.Has("-pt_lens_cone_pinhole")) m_flags |= 33554432u; // Diagnostic: the pinhole ray cone.
    if(cmd.Has("-pt_path_ray_cone")) m_flags |= 2097152u;
    if(cmd.Has("-pt_alpha_ray_cone")) m_flags |= 67108864u | 2097152u;
    if(cmd.Has("-pt_area_reconnect")) m_flags |= 134217728u | 4194304u | 32768u | 16384u | 8u;
    if(cmd.Has("-pt_area_radiance_cache") || cmd.Has("-pt_area_radiance_corrupt"))
        m_flags |= 1073741824u | 134217728u | 4194304u | 32768u | 16384u | 8u;
    if(cmd.Has("-pt_area_radiance_corrupt")) m_flags |= 2147483648u; // Negative control only.
    const float reconnectWeight=Settings::Pass::ptAreaReconnectWeight();
    m_areaReconnectWeight=std::isfinite(reconnectWeight)?std::clamp(reconnectWeight,0.f,1.f):1.f;
    if(m_flags&134217728u) LOG_INFO("[AREA_RECONNECT_SETTINGS] weight=%.9g",m_areaReconnectWeight);
    if(cmd.Has("-pt_reconnect_probe")) m_flags |= 268435456u | 8u;
    if(cmd.Has("-pt_reconnect_unit_jacobian")) m_flags |= 536870912u;
    // Area v2 (MIS-combined random-replay + primary-hit reconnection shifts, pairwise temporal,
    // defensive K-neighbour spatial) is the Area implementation; -pt_area_legacy and the
    // single-purpose experiment flags select the pre-v2 kernels for their evidence.
    const bool legacyArea=cmd.Has("-pt_area_legacy") || (m_flags&(65536u|262144u|4194304u|134217728u|1073741824u));
    m_areaV2=(m_flags&16384u) && !legacyArea;
    if(m_areaV2)
    {
        m_flags |= 65536u | 4194304u;              // film rotations + selected primary context banks
        if(m_flags&32768u) m_flags |= 1073741824u; // same-frame selected radiance for the spatial pass
    }
    // A pinned motion (-pt_area_motion, raytracing.ptAreaMotion's flag) holds without an Area
    // mode.
    m_areaMotion=(m_areaMotionPinned && m_areaMotionRequested) || (m_flags&65536u) || (m_areaModeRequested>0 && m_areaMotionRequested);
    if(m_areaMotion) m_flags |= 16384u;
    // Spatial reuse only for short histories (-pt_area_spatial_always lifts the gate).
    m_spatialGate=!cmd.Has("-pt_area_spatial_always");
    m_areaHistoryCap=std::clamp(m_areaCapRequested,1u,64u);
    LOG_INFO("[AREA_SETTINGS] mode=%u motion=%u cap=%u",(m_flags&32768u)?2u:((m_flags&16384u)?1u:0u),m_areaMotion?1u:0u,m_areaHistoryCap);
    m_cameraSettingsLogged=false;
    SetCameraSettings(false,false,false,0.f,5.f);
    if (cmd.Has("-rt_hit_probe")) m_flags |= 136u;
    if (cmd.Has("-rt_hit_probe_wrong_instance")) m_flags |= 392u;
    m_statsBytes = ((m_flags & 16u) ? 7 + 2*7*128 : 7) * sizeof(uint32_t);
    if (m_flags & 128u) m_statsBytes = (7 + 2*7*128 + 16*32) * sizeof(uint32_t);
    if (m_flags & 16384u) m_statsBytes = (7 + 2*7*128 + 16*32 + 6) * sizeof(uint32_t);
    if (m_flags & 65536u) m_statsBytes = (7 + 2*7*128 + 16*32 + 9) * sizeof(uint32_t);
    if(m_flags&262144u) m_statsBytes=(7+2*7*128+16*32+12)*sizeof(uint32_t);
    if(m_flags&4194304u) m_statsBytes=(7+2*7*128+16*32+15)*sizeof(uint32_t);
    if(m_flags&16777216u) m_statsBytes=(7+2*7*128+16*32+24)*sizeof(uint32_t);
    if(m_flags&134217728u) m_statsBytes=(7+2*7*128+16*32+28)*sizeof(uint32_t);
    if(m_flags&268435456u) m_statsBytes=(7+2*7*128+16*32+60)*sizeof(uint32_t);
    if(m_areaV2) m_statsBytes=(7+2*7*128+16*32+64+16)*sizeof(uint32_t);
    if (m_flags & 16u)
        m_statsBytes = std::max<uint32_t>(m_statsBytes, kMediaProbeEnd * sizeof(uint32_t));
    if (!gfx.GetCapabilities().inlineRaytracing) return;
    RHI::PipelineLayoutBuilder b("PathTrace");
    b.RootCBV(kRootCB, 0)
     .RootSRV(kRootTLAS, 0).RootSRV(kRootInstances, 1).RootSRV(kRootAlpha, 2)
     .Table(kRootBuffers,  { RHI::SrvRange(0, 16384, 1) })   // t0 space1: the bindless buffers
     .Table(kRootTextures, { RHI::SrvRange(0, 16384, 2) })   // t0 space2: the bindless textures
     .Table(kRootResult,   { RHI::UavRange(0) })
     .Table(kRootAccum,    { RHI::UavRange(1) })
     .Table(kRootStats,    { RHI::UavRange(2) });
    RHI::SamplerDesc linearWrap{};   // MIN_MAG_MIP_LINEAR, WRAP, MaxLOD FLT_MAX
    linearWrap.maxAnisotropy  = 1;
    linearWrap.comparisonFunc = RHI::ComparisonFunc::NONE;   // D3D12's 0: no comparison
    b.Sampler(linearWrap, 1);
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    { LOG_ERROR("PathTracePass: pipeline layout failed"); return; }
    // The pipelines, counters and constants are built on first use
    // (BuildPipelines, from EnsureResources).
    m_pipelinesFailed = false;
    m_frontFailed = false;
    m_nprFailed = false;
    m_mediaFailed = false;
    m_dnFailed = false;
    m_fogDnFailed = false;
    m_motionFailed = false;
}

// The image's entries: Area v2's temporal and spatial kernels, the pre-v2 Area kernels the
// experiment flags select, or the plain one.
const char* PathTracePass::MainEntry() const
{
    return m_areaV2 ? "AreaTemporalV2" :
        ((m_flags&1073741824u)?((m_flags&262144u)?"AreaOverlapCached":"AreaCached"):((m_flags&262144u)?"AreaOverlap":"main"));
}

const char* PathTracePass::SpatialEntry() const
{
    return m_areaV2 ? "AreaSpatialV2" :
        ((m_flags&1073741824u)?"AreaReconnectCached":((m_flags&134217728u)?"AreaReconnect":"AreaSpatial"));
}

bool PathTracePass::BuildPipelines()
{
    if (m_pso.IsValid()) return true;
    if (m_pipelinesFailed || !m_layout.IsValid() || !m_gfx) return false;
    m_pipelinesFailed = true;   // cleared once everything is built
    IGraphicsDevice& gfx = *m_gfx;
    const auto start = std::chrono::steady_clock::now();
    // One pipeline on the pass's layout; an empty kernel is a failure.
    auto make = [&](const std::vector<uint8_t>& dxil, RHI::PipelineState& out)
    { return !dxil.empty() && ComputePipelineBatch::Create(gfx, m_layout, &dxil, 1, &out); };
    auto shader = ShaderDiskCache::LoadOrCompile("shaders/PathTrace.cs.hlsl", MainEntry(), RHI::ShaderStage::CS, {}, "PathTracePass");
    if (shader.empty()) return false;
    if (!make(shader, m_pso))
    { LOG_ERROR("PathTracePass: PSO creation failed"); return false; }
    if(m_flags&32768u)
    {
        auto spatial=ShaderDiskCache::LoadOrCompile("shaders/PathTrace.cs.hlsl",SpatialEntry(),RHI::ShaderStage::CS,{},"PathTraceAreaSpatial");
        if(!make(spatial,m_areaSpatialPso))
        { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: Area spatial pipeline failed"); return false; }
    }
    auto clear = ShaderDiskCache::LoadOrCompile("shaders/PathTrace.cs.hlsl", "ClearCounters", RHI::ShaderStage::CS, {}, "PathTracePass");
    if (!make(clear, m_clearStats))
    { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: counter pipeline failed"); return false; }
    if (m_flags & 128u)
    {
        auto probe = ShaderDiskCache::LoadOrCompile("shaders/RTHitProbe.cs.hlsl", "main", RHI::ShaderStage::CS, {}, "RTHitProbe");
        if (!make(probe, m_hitProbe))
        { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: hit probe pipeline failed"); return false; }
    }
    RHI::GPUBufferDesc bd{};
    if (m_flags & 16u)
    {
        auto probe = ShaderDiskCache::LoadOrCompile("shaders/BSDFProbe.cs.hlsl", "main", RHI::ShaderStage::CS, {}, "BSDFProbe");
        if (!make(probe, m_probe))
        { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: BSDF probe pipeline failed"); return false; }
        probe = ShaderDiskCache::LoadOrCompile("shaders/BSDFProbe.cs.hlsl", "Clear", RHI::ShaderStage::CS, {}, "BSDFProbe");
        if (!make(probe, m_clearProbe))
        { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: BSDF probe clear failed"); return false; }
        const char* dielectricEntries[3] = { "DielectricClear", "Dielectric", "DielectricReduce" };
        RHI::PipelineState* dielectricPsos[3] = { &m_dielectricProbeClear, &m_dielectricProbe, &m_dielectricProbeReduce };
        for (int i = 0; i < 3; ++i)
        {
            probe = ShaderDiskCache::LoadOrCompile("shaders/BSDFProbeDielectric.cs.hlsl", dielectricEntries[i],
                                                   RHI::ShaderStage::CS, {}, "BSDFProbe");
            if (!make(probe, *dielectricPsos[i]))
            { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: dielectric BSDF probe pipeline failed"); return false; }
        }
        const char* mediaEntries[5] = { "MediaClear", "MediaHg", "MediaFreeFlight", "MediaEquiAngular", "MediaReduce" };
        RHI::PipelineState* mediaPsos[5] = { &m_mediaProbeClear, &m_mediaProbeHg, &m_mediaProbeFreeFlight,
                                             &m_mediaProbeEquiAngular, &m_mediaProbeReduce };
        for (int i = 0; i < 5; ++i)
        {
            probe = ShaderDiskCache::LoadOrCompile("shaders/MediaProbe.cs.hlsl", mediaEntries[i],
                                                   RHI::ShaderStage::CS, {}, "BSDFProbe");
            if (!make(probe, *mediaPsos[i]))
            { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: media probe pipeline failed"); return false; }
        }
    }
    bd.size = m_statsBytes; bd.stride = sizeof(uint32_t);
    bd.usage = RHI::Usage::DEFAULT; bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    if (!gfx.CreateBuffer(bd,m_stats)) { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: counter allocation failed"); return false; }
    m_statsState = RHI::ResourceState::UNDEFINED;
    if (m_flags & 8u)
    {
        bd.usage = RHI::Usage::READBACK; bd.bindFlags = RHI::BindFlag::NONE;
        for (uint32_t i=0; i<RHI::kFramesInFlight; ++i)
            if (!gfx.CreateBuffer(bd,m_readback[i]) || !(m_mapped[i] = static_cast<const uint32_t*>(gfx.MapBuffer(m_readback[i]))))
            { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: counter readback failed"); return false; }
    }
    if (!m_cb.Create(gfx, "PT.Constants")) { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: constants failed"); return false; }
    if ((m_flags&32768u) && !m_areaSpatialCb.Create(gfx,"PT.AreaSpatialConstants"))
    { gfx.DestroyPipelineState(m_pso); LOG_ERROR("PathTracePass: Area spatial constants failed"); return false; }
    m_pipelinesFailed = false;
    m_pipelineMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    LOG_INFO("PathTracePass: pipelines built on first use (%.0f ms)", m_pipelineMs);
    return true;
}

// Both PT cores are kept, split by use. "auto", the default request, runs RTXDI's ReSTIR PT
// for real-time path tracing and this pass's own path tracer for every use RTXDI's core does
// not serve: the uncapped reference (the ground truth), an explicit engine bounce count
// (-pt_bounces: the engine path set the A/B tools match against), Area reuse (ptAreaMode /
// -pt_area_*), and camera-primary rays - a thin lens, film jitter: RTXDI's paths start at the
// GBuffer. An explicit request runs as asked, except rtxdi with a thin lens, which would drop
// the depth of field, or with -pt_gi_shell_model, which would drop that model.
ReSTIR::Backend PathTracePass::ResolveRequest(ReSTIR::Backend requested, const char*& why) const
{
    why = nullptr;
    if (requested == ReSTIR::Backend::Rtxdi && m_lensRadius > 0.f)
    { why = "thin lens: rtxdi starts at the GBuffer"; return ReSTIR::Backend::Area; }
    // A model of this pass's own bounce vertices (a reference): RTXDI's core would drop it, as
    // asked or by auto.
    if (requested != ReSTIR::Backend::Area && m_giShellModel)
    { why = "-pt_gi_shell_model: the engine path tracer's model"; return ReSTIR::Backend::Area; }
    if (requested != ReSTIR::Backend::Auto) return requested;
    // The render path decides. Path Trace (Reference) is the engine path tracer whatever its
    // bounce setting; Path Trace (Real-time) is RTXDI's core unless a command-line pin asks for
    // the engine's: an explicit -pt_reference / -pt_bounces, or Area / camera-primary flags (the
    // editor's camera-ray modes are held off there: Renderer::PTCameraModesHeld).
    if (!m_realtime) { why = "auto: reference path tracing"; return ReSTIR::Backend::Area; }
    if (m_maxBounces == 0 && !m_referenceFromSettings)
    { why = "auto: -pt_reference sets the engine path tracer"; return ReSTIR::Backend::Area; }
    if (m_bouncesCli) { why = "auto: -pt_bounces sets the engine path tracer"; return ReSTIR::Backend::Area; }
    if (m_flags & 16384u) { why = "auto: area reuse requested"; return ReSTIR::Backend::Area; }
    if (m_flags & (512u | 1024u)) { why = "auto: camera-primary rays (thin lens, film jitter)"; return ReSTIR::Backend::Area; }
    why = "auto: real-time path tracing";
    return ReSTIR::Backend::Rtxdi;
}

void PathTracePass::SetRealtimePreset(bool on)
{
    // The render path's; -pt_realtime picks the path at boot (ApplyRenderPathFlags), so a Lua
    // path switch still changes the core.
    if (on == m_realtime) return;
    m_realtime = on;
    PTRtxdi::SetRealtimePreset(on);
    // The core reads the preset when it is made: remake it at this frame
    // boundary, as a backend switch does (ApplyBackend).
    if (m_rtxdi && m_gfx)
    {
        m_gfx->WaitIdleAndReleaseDeferred();
        m_rtxdi->Release(*m_gfx);
        m_rtxdi->ReleasePipelines(*m_gfx);
        m_rtxdi.reset();
    }
    m_backendDirty = true;
    ResetHistory();
    LOG_INFO("[PT_REALTIME] preset=%u", on ? 1u : 0u);
}

void PathTracePass::SetDirectLight(PTRtxdi::DirectLight d)
{
    const PTRtxdi::DirectLight before = PTRtxdi::EffectiveDirectLight();
    const bool settingChanged = PTRtxdi::GetDirectLight() != d;
    PTRtxdi::SetDirectLight(d);
    const PTRtxdi::DirectLight now = PTRtxdi::EffectiveDirectLight();
    if (!settingChanged) return;
    LOG_INFO("[PT_DIRECT_LIGHT] setting=%s effective=%s", PTRtxdi::DirectLightName(d), PTRtxdi::DirectLightName(now));
    // A CLI pin (-rtxdi_pt_di / -rtxdi_pt_unified) keeps the core as it is.
    if (now == before) return;
    if (m_rtxdi && m_gfx)
    {
        m_gfx->WaitIdleAndReleaseDeferred();
        m_rtxdi->Release(*m_gfx);
        m_rtxdi->ReleasePipelines(*m_gfx);
        m_rtxdi.reset();
    }
    m_backendDirty = true;
    ResetHistory();
}

void PathTracePass::SetBackend(ReSTIR::Backend requested)
{
    if (!ReSTIR::IsLegal(ReSTIR::Stage::PT, requested)) requested = ReSTIR::Backend::Area;
    const char* why = nullptr;
    const ReSTIR::Backend resolved = ResolveRequest(requested, why);
    if (requested != m_requested || resolved != m_resolved || why != m_resolveWhy)
    { m_requested = requested; m_resolved = resolved; m_resolveWhy = why; m_backendDirty = true; }
}

void PathTracePass::ApplyBackend()
{
    if (!m_backendDirty) return;
    m_backendDirty = false;
    const ReSTIR::Backend previous = m_status.effective;
    m_status.requested = m_requested;
    m_status.requestedName = ReSTIR::BackendName(m_requested);
    const bool native16 = m_gfx && m_gfx->GetCapabilities().native16BitShaderOps;
    const char* inert = m_resolved == ReSTIR::Backend::Rtxdi ? ReSTIR::RtxdiInertReason(ReSTIR::Stage::PT, native16) : nullptr;
    bool rtxdi = m_resolved == ReSTIR::Backend::Rtxdi && !inert;
    if (rtxdi && !m_rtxdi) m_rtxdi = PTRtxdi::MakeCore();
    if (rtxdi && (!m_rtxdi || !m_gfx || !m_rtxdi->BuildPipelines(*m_gfx)))
    { rtxdi = false; inert = m_rtxdi ? "rtxdi pipeline build failed" : "rtxdi sdk not in this build"; }
    uint64_t released = 0;
    if (!rtxdi && m_rtxdi && m_gfx)
    {
        // Leaving the mode releases its working set at this frame boundary.
        released = m_rtxdi->ResidentBytes();
        m_gfx->WaitIdleAndReleaseDeferred();
        m_rtxdi->Release(*m_gfx);
        m_rtxdi->ReleasePipelines(*m_gfx);
        m_rtxdi.reset();
    }
    m_status.effective = rtxdi ? ReSTIR::Backend::Rtxdi : ReSTIR::Backend::Area;
    m_status.effectiveName = ReSTIR::BackendName(m_status.effective);
    // Why it runs what it runs: an unavailable core first, then the use auto
    // read (or the thin-lens fallback), else the request as asked.
    m_status.reason = inert ? inert : (m_resolveWhy ? m_resolveWhy
        : (m_requested == m_status.effective ? "selected" : "rtxdi core not in this build"));
    m_status.ignored.clear();
    if (previous != m_status.effective)
    {
        LOG_INFO("[RESTIR_SWITCH] pass=PT.Trace from=%s to=%s releasedBytes=%llu", ReSTIR::BackendName(previous),
                 m_status.effectiveName, static_cast<unsigned long long>(released));
        ResetHistory();
    }
    m_rtxdiLogged = false;
    m_logBackend = true;
}

uint64_t PathTracePass::AreaResidentBytes() const
{
    if (m_rtxdi) return m_rtxdi->ResidentBytes();
    // The Area banks OnCompile allocates (two of each) and the selected-radiance scratch.
    if (!(m_flags & 16384u) || !m_hist->areaHistory[0].IsValid()) return 0;
    uint64_t perTexel = 16;
    if (m_flags & 65536u) perTexel += 4;
    if (m_flags & 4194304u) perTexel += 32;
    if (m_areaV2) perTexel += 8;
    uint64_t bytes = uint64_t(m_ws->w) * m_ws->h * perTexel * 2;
    if (m_ws->areaRadiance.IsValid()) bytes += uint64_t(m_ws->w) * m_ws->h * 16;
    return bytes;
}

void PathTracePass::SetRayReconstruction(bool enabled)
{
    // Replay evaluates multiple primaries; raster-motion RR guides are invalid. The rtxdi mode's
    // Shade writes the same guides.
    if(m_flags&16384u) enabled=false;
    if(m_rrEnabled==enabled) return;
    m_rrEnabled=enabled;
    if(enabled && m_ws->reference && m_gfx)
    { m_gfx->WaitIdleAndReleaseDeferred(); m_ws->reference.reset(); }
    ResetHistory();
    LOG_INFO("[PT_RR] enabled=%u accumulation=%u nrdReference=%u",enabled,!enabled,!enabled && bool(m_flags&32u));
}

void PathTracePass::EnsureResources(bool wanted)
{
    m_wanted = wanted;
    if(m_gfx && !wanted && (m_hist->areaHistory[0].IsValid() || m_hist->areaHistory[1].IsValid()))
    {
        m_gfx->WaitIdleAndReleaseDeferred();
        for(auto& history:m_hist->areaHistory) { if(history.IsValid()) m_gfx->DestroyTexture(history); history={}; }
        for(auto& film:m_hist->areaFilm) { if(film.IsValid()) m_gfx->DestroyTexture(film); film={}; }
        for(auto& t:m_hist->areaPrimary) { if(t.IsValid()) m_gfx->DestroyTexture(t); t={}; }
        for(auto& t:m_hist->areaGeometry) { if(t.IsValid()) m_gfx->DestroyTexture(t); t={}; }
        for(auto& t:m_hist->areaLens) { if(t.IsValid()) m_gfx->DestroyTexture(t); t={}; }
        if(m_ws->areaRadiance.IsValid()) m_gfx->DestroyTexture(m_ws->areaRadiance);
        m_ws->areaRadiance={}; m_ws->areaRadianceState=RHI::ResourceState::UNDEFINED;
        ResetHistory();
        LOG_INFO("[AREA] released=1");
    }
    if(m_gfx && !wanted && m_rtxdi && m_rtxdi->IsAllocated())
    {
        m_gfx->WaitIdleAndReleaseDeferred();
        m_rtxdi->Release(*m_gfx);
        ResetHistory();
        LOG_INFO("[PT_RTXDI] released=1");
    }
    if(m_gfx && (!wanted || !m_rrEnabled) && m_rrGuides[0].IsValid())
    {
        m_gfx->WaitIdleAndReleaseDeferred();
        for(auto& guide:m_rrGuides) { if(guide.IsValid()) m_gfx->DestroyTexture(guide); guide={}; }
    }
    if(m_gfx && (!wanted || !m_frontWanted) && m_front.IsValid()) ReleaseFrontReflection(*m_gfx);
    if(m_gfx && (!wanted || !m_nprWanted) && m_nprFinal.IsValid()) ReleaseNprLighting(*m_gfx);
    if(m_gfx && (!wanted || !m_dnWanted) && m_hist->dnSampleMoment.IsValid()) ReleaseDenoiser(*m_gfx);
    if (wanted && m_gfx)
    {
        ApplyBackend();
        BuildPipelines();
        // PathTraceResult is on demand (Renderer::Compile); backed on the first
        // frame this pass is wanted with a pipeline, sticky from then on.
        if (m_pso.IsValid()) m_graph.EnsurePhysicalTexture(m_result, *m_gfx);
        OnCompile(*m_gfx);
        if (UsesRtxdi() && m_rtxdi && m_ws->w && m_ws->h && !m_rtxdi->Allocate(*m_gfx, m_ws->w, m_ws->h))
            m_status.reason = "rtxdi allocation failed";
        m_status.pipelineMs = m_pipelineMs;
        m_status.residentBytes = AreaResidentBytes();
        if (m_logBackend && (m_pso.IsValid() || m_pipelinesFailed))
        {
            if (m_pipelinesFailed) m_status.reason = "pipeline build failed";
            LOG_INFO("[RESTIR_BACKEND] stage=PT requested=%s effective=%s reason=%s pipelineMs=%.1f residentBytes=%llu ignored=-",
                m_status.requestedName, m_status.effectiveName, m_status.reason, m_status.pipelineMs,
                static_cast<unsigned long long>(m_status.residentBytes));
            m_logBackend = false;
        }
    }
}

void PathTracePass::SetSettings(uint32_t maxBounces, bool reference, bool bouncesPinned, bool referencePinned)
{
    const auto& cmd=Sys::CommandLine::Get();
    // -pt_bounces / -pt_reference pin raytracing.ptMaxBounces / ptReferenceArm. Pinned values
    // keep the flags' precedence: a pinned reference, then pinned bounces, then the settings.
    uint32_t effective=reference ? 0u : std::clamp(maxBounces,1u,64u);
    if (bouncesPinned) effective=std::clamp(maxBounces,1u,64u);
    if (referencePinned && reference) effective=0u;
    m_referenceFromSettings=reference && !bouncesPinned && !referencePinned;
    m_bouncesCli=bouncesPinned;
    m_bounceCapCli=cmd.Has("-pt_no_bounce_cap");
    if (effective!=m_maxBounces) { m_maxBounces=effective; ResetHistory(); }
}

void PathTracePass::SetAreaSettings(uint32_t mode,bool motion,uint32_t historyCap,
    uint32_t spatialK,float spatialRadius,bool reconnect,bool motionPinned)
{
    const auto& command=Sys::CommandLine::Get();
    // -pt_area_spatial_k / -pt_area_spatial_radius / -pt_area_v2_reconnect /
    // -pt_area_no_reconnect are the settings' flag aliases.
    spatialK=std::clamp(spatialK,1u,4u);
    spatialRadius=std::isfinite(spatialRadius)?std::clamp(spatialRadius,1.f,32.f):6.f;
    m_areaNoIdentity=command.Has("-pt_area_no_identity");
    const float confidence=Settings::Pass::ptAreaSpatialConfidence();
    m_areaSpatialConfidence=std::isfinite(confidence)?std::clamp(confidence,0.f,4.f):1.f;
    if(spatialK!=m_areaSpatialK || spatialRadius!=m_areaSpatialRadius || reconnect!=m_areaV2Reconnect)
    {
        m_areaSpatialK=spatialK; m_areaSpatialRadius=spatialRadius; m_areaV2Reconnect=reconnect;
        LOG_INFO("[AREA_V2_SETTINGS] spatialK=%u spatialRadius=%.3f reconnect=%u confidence=%.3f",
            m_areaSpatialK,m_areaSpatialRadius,m_areaV2Reconnect?1u:0u,m_areaSpatialConfidence);
    }
    mode=std::min(mode,2u); historyCap=std::clamp(historyCap,1u,64u);
    const bool rebuild=mode!=m_areaModeRequested || motion!=m_areaMotionRequested || motionPinned!=m_areaMotionPinned;
    const bool capChanged=historyCap!=m_areaCapRequested;
    m_areaModeRequested=mode; m_areaMotionRequested=motion; m_areaCapRequested=historyCap; m_areaMotionPinned=motionPinned;
    // Settings are consumed before graph recording. Reuse the shader-reload
    // lifecycle so counter/readback sizes, PSOs and reservoir banks agree.
    if(rebuild && m_gfx) ReloadShaders(*m_gfx);
    else if(capChanged)
    {
        m_areaHistoryCap=std::clamp(historyCap,1u,64u);
        ResetHistory();
        LOG_INFO("[AREA_SETTINGS] mode=%u motion=%u cap=%u",(m_flags&32768u)?2u:((m_flags&16384u)?1u:0u),m_areaMotion?1u:0u,m_areaHistoryCap);
    }
}

void PathTracePass::SetCameraSettings(bool cameraPrimary,bool filmJitter,bool primaryRayCone,float lensRadius,float focusDistance)
{
    // -pt_camera_primary / -pt_film_jitter / -pt_primary_ray_cone / -pt_lens_radius /
    // -pt_focus_distance are the settings' flag aliases: the Renderer hands their values in, a
    // pin past the reference path's hold (DevPin).
    lensRadius=std::isfinite(lensRadius)?std::clamp(lensRadius,0.f,1.f):0.f;
    focusDistance=std::isfinite(focusDistance)?std::clamp(focusDistance,.01f,10000.f):5.f;
    cameraPrimary=cameraPrimary || lensRadius>0 || (m_flags&16384u);
    filmJitter=filmJitter || (m_flags&16384u);
    primaryRayCone=primaryRayCone || (m_flags&2097152u);
    const uint32_t cameraFlags=(cameraPrimary?512u:0u)|(filmJitter?1024u:0u)|(primaryRayCone?2048u:0u);
    const uint32_t updated=(m_flags&~(512u|1024u|2048u))|cameraFlags;
    const bool changed=updated!=m_flags || lensRadius!=m_lensRadius || focusDistance!=m_focusDistance;
    m_flags=updated; m_lensRadius=lensRadius; m_focusDistance=focusDistance;
    if(changed) ResetHistory();
    if(changed || !m_cameraSettingsLogged)
    {
        LOG_INFO("[PTCAMERA] enabled=%u filmJitter=%u lensRadius=%.6f focusDistance=%.6f primaryRayCone=%u",
            cameraPrimary,filmJitter,m_lensRadius,m_focusDistance,primaryRayCone);
        m_cameraSettingsLogged=true;
    }
}

void PathTracePass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this, this);
    if (view.Slot() == 0)
    {
        m_ws = &m_main;
        m_mainHist = m_hist;
    }
    else
    {
        ViewWork& work = view.Data<ViewWork>(this);
        work.owner = this;
        m_ws = &work.ws;
    }
}

void PathTracePass::ViewWork::Release(IGraphicsDevice& gfx)
{
    if (ws.areaRadiance.IsValid()) gfx.DestroyTexture(ws.areaRadiance);
    if (owner && ws.reference) owner->m_retiredNrd.emplace_back(std::move(ws.reference), RHI::kFramesInFlight + 1u);
    ws = WorkingSet{};
}

void PathTracePass::History::Release(IGraphicsDevice& gfx)
{
    // Shutdown destroyed them already (the lifecycle's Shutdown runs first):
    // anything still valid here is a Shutdown that never ran.
    for (RHI::Texture* t : { &accum, &areaHistory[0], &areaHistory[1], &areaFilm[0], &areaFilm[1],
                             &areaPrimary[0], &areaPrimary[1], &areaGeometry[0], &areaGeometry[1],
                             &areaLens[0], &areaLens[1], &nprHistory[0], &nprHistory[1], &nprMeta[0], &nprMeta[1], &fog,
                             &dnHistory[0], &dnHistory[1], &dnMoments[0], &dnMoments[1], &dnMeta[0], &dnMeta[1],
                             &dnFilter[0], &dnFilter[1], &dnSampleMoment,
                             &fogDnHistory[0], &fogDnHistory[1], &fogDnMeta[0], &fogDnMeta[1], &fogDnOut })
    {
        if (t->IsValid()) gfx.DestroyTexture(*t);
        *t = {};
    }
    fogValid = fogStale = false;
    fogW = fogH = 0;
    dnHistoryValid = dnMomentValid = false;
    dnW = dnH = 0;
    fogDnOutState = RHI::ResourceState::UNDEFINED;
    fogDnHistoryValid = false;
    fogDnW = fogDnH = 0;
}

void PathTracePass::OnCompile(IGraphicsDevice& gfx)
{
    // The main view's (the graph's compile and the service point); a secondary view's is made in its
    // segment (EnsureViewResources).
    if (m_ws != &m_main) return;
    BuildWorkingSet(gfx, gfx.GetRenderWidth(), gfx.GetRenderHeight());
}

void PathTracePass::EnsureViewResources(uint32_t w, uint32_t h, bool wanted)
{
    if (m_ws == &m_main || !m_gfx || !m_hist) return;
    WorkingSet& ws = *m_ws;
    // The RTXDI mode is the main view's: a view path-traces only with this pass's own path tracer.
    if (!wanted || UsesRtxdi())
    {
        if (ws.w || ws.areaRadiance.IsValid() || ws.reference || m_hist->accum.IsValid())
        {
            if (ws.areaRadiance.IsValid()) m_gfx->DestroyTexture(ws.areaRadiance);
            ws.areaRadiance = {}; ws.areaRadianceState = RHI::ResourceState::UNDEFINED;
            if (ws.reference) m_retiredNrd.emplace_back(std::move(ws.reference), RHI::kFramesInFlight + 1u);
            m_hist->Release(*m_gfx);
            ws.w = ws.h = 0;
            ResetHistory();
        }
        return;
    }
    // The main view's settings its history was traced with: a change starts it over, as the main view's
    // setters do.
    const uint32_t traced[5] = { m_flags, m_maxBounces, m_areaHistoryCap, m_areaSpatialK,
                                 (m_areaV2 ? 1u : 0u) | (m_areaV2Reconnect ? 2u : 0u) | (m_areaMotion ? 4u : 0u) |
                                 (static_cast<uint32_t>(m_status.effective) << 8) };
    const float tracedF[5] = { m_lensRadius, m_focusDistance, m_areaSpatialRadius, m_areaReconnectWeight,
                               m_constantEnvironment };
    if (std::memcmp(traced, ws.traced, sizeof(traced)) != 0 || std::memcmp(tracedF, ws.tracedF, sizeof(tracedF)) != 0)
    {
        ResetHistory();
        std::memcpy(ws.traced, traced, sizeof(traced));
        std::memcpy(ws.tracedF, tracedF, sizeof(tracedF));
    }
    BuildWorkingSet(*m_gfx, w, h);
}

void PathTracePass::BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (!m_wanted || !m_pso.IsValid()) return;
    if(!w || !h) return;
    const bool main = m_ws == &m_main;
    const bool rrEnabled = main && m_rrEnabled;   // ray reconstruction is the main view's
    if((m_flags&16384u) && !UsesRtxdi() && (w!=m_ws->w || h!=m_ws->h || ((m_flags&1073741824u) && !m_ws->areaRadiance.IsValid()) || !m_hist->areaHistory[0].IsValid() || !m_hist->areaHistory[1].IsValid() ||
        (m_areaV2 && (!m_hist->areaLens[0].IsValid() || !m_hist->areaLens[1].IsValid())) ||
        ((m_flags&65536u) && (!m_hist->areaFilm[0].IsValid() || !m_hist->areaFilm[1].IsValid())) ||
        ((m_flags&4194304u) && (!m_hist->areaPrimary[0].IsValid() || !m_hist->areaPrimary[1].IsValid() || !m_hist->areaGeometry[0].IsValid() || !m_hist->areaGeometry[1].IsValid()))))
    {
        if (main) gfx.WaitIdleAndReleaseDeferred();   // a view's segment: the destroys are deferred
        for(uint32_t i=0;i<2;++i)
        {
            if(m_hist->areaHistory[i].IsValid()) gfx.DestroyTexture(m_hist->areaHistory[i]);
            m_hist->areaHistory[i]={};
            RHI::TextureDesc history{};
            history.width=w; history.height=h; history.format=RHI::Format::R32G32B32A32_UINT;
            history.bindFlags=RHI::BindFlag::UNORDERED_ACCESS|RHI::BindFlag::SHADER_RESOURCE;
            history.usage=RHI::Usage::DEFAULT; history.layout=RHI::ResourceState::UNORDERED_ACCESS;
            history.debugName="PT.AreaReservoir";
            if(!gfx.CreateTexture(history,m_hist->areaHistory[i])) { LOG_ERROR("PT: Area reservoir allocation failed"); return; }
            if(m_flags&65536u)
            {
                if(m_hist->areaFilm[i].IsValid()) gfx.DestroyTexture(m_hist->areaFilm[i]);
                m_hist->areaFilm[i]={};
                history.format=RHI::Format::R32_UINT; history.debugName="PT.AreaFilmRotation";
                if(!gfx.CreateTexture(history,m_hist->areaFilm[i])) { LOG_ERROR("PT: Area film allocation failed"); return; }
            }
            if(m_flags&4194304u)
            {
                if(m_hist->areaPrimary[i].IsValid()) gfx.DestroyTexture(m_hist->areaPrimary[i]);
                if(m_hist->areaGeometry[i].IsValid()) gfx.DestroyTexture(m_hist->areaGeometry[i]);
                m_hist->areaPrimary[i]={}; m_hist->areaGeometry[i]={};
                history.format=RHI::Format::R32G32B32A32_UINT; history.debugName="PT.AreaPrimaryHit";
                if(!gfx.CreateTexture(history,m_hist->areaPrimary[i])) { LOG_ERROR("PT: Area primary allocation failed"); return; }
                history.debugName="PT.AreaPrimaryGeometry";
                if(!gfx.CreateTexture(history,m_hist->areaGeometry[i])) { LOG_ERROR("PT: Area geometry allocation failed"); return; }
            }
            if(m_areaV2)
            {
                if(m_hist->areaLens[i].IsValid()) gfx.DestroyTexture(m_hist->areaLens[i]);
                m_hist->areaLens[i]={};
                history.format=RHI::Format::R32G32_FLOAT; history.debugName="PT.AreaLens";
                if(!gfx.CreateTexture(history,m_hist->areaLens[i])) { LOG_ERROR("PT: Area lens allocation failed"); return; }
            }
            m_hist->areaStates[i]=history.layout;
        }
        if(m_flags&1073741824u)
        {
            if(m_ws->areaRadiance.IsValid()) gfx.DestroyTexture(m_ws->areaRadiance);
            m_ws->areaRadiance={};
            RHI::TextureDesc scratch{};
            scratch.width=w; scratch.height=h; scratch.format=RHI::Format::R32G32B32A32_FLOAT;
            scratch.bindFlags=RHI::BindFlag::UNORDERED_ACCESS|RHI::BindFlag::SHADER_RESOURCE;
            scratch.usage=RHI::Usage::DEFAULT; scratch.layout=RHI::ResourceState::UNORDERED_ACCESS;
            scratch.debugName="PT.AreaSelectedRadiance";
            if(!gfx.CreateTexture(scratch,m_ws->areaRadiance)) { LOG_ERROR("PT: Area radiance allocation failed"); return; }
            m_ws->areaRadianceState=scratch.layout;
            if (main) LOG_INFO("[AREA_RADIANCE] allocated=1 width=%u height=%u bytes=%llu",w,h,static_cast<unsigned long long>(w)*h*16);
        }
        m_hist->areaWrite=0; ResetHistory();
        if (main)   // the [AREA] lines are the main view's
        {
            LOG_INFO("[AREA] allocated=1 width=%u height=%u bytes=%llu historyCap=%u mapping=%s",w,h,
                static_cast<unsigned long long>(w)*h*(m_areaV2?48:32),m_areaHistoryCap,m_areaV2?"v2(replay+reconnection MIS)":"randomReplay");
            if(m_flags&4194304u) LOG_INFO("[AREA_CONTEXT] allocated=1 bytes=%llu",static_cast<unsigned long long>(w)*h*64);
            if(m_flags&65536u) LOG_INFO("[AREA_FILM] allocated=1 bytes=%llu",static_cast<unsigned long long>(w)*h*8);
        }
    }
    if(rrEnabled && (w!=m_ws->w || h!=m_ws->h || !m_rrGuides[0].IsValid() || !m_rrGuides[1].IsValid() || !m_rrGuides[2].IsValid() || !m_rrGuides[3].IsValid()))
    {
        gfx.WaitIdleAndReleaseDeferred();
        for(uint32_t i=0;i<4;++i)
        {
            if(m_rrGuides[i].IsValid()) gfx.DestroyTexture(m_rrGuides[i]);
            m_rrGuides[i]={};
            RHI::TextureDesc guide{};
            guide.width=w; guide.height=h; guide.format=i==3?RHI::Format::R32_FLOAT:RHI::Format::R16G16B16A16_FLOAT;
            guide.bindFlags=RHI::BindFlag::UNORDERED_ACCESS|RHI::BindFlag::SHADER_RESOURCE;
            guide.usage=RHI::Usage::DEFAULT; guide.layout=RHI::ResourceState::UNORDERED_ACCESS;
            guide.debugName="PT.RRGuide";
            if(!gfx.CreateTexture(guide,m_rrGuides[i])) { LOG_ERROR("PT: RR guide allocation failed"); return; }
            m_rrGuideStates[i]=guide.layout;
        }
    }
    if (main)   // the front reflection, the toon lighting and the motion probe are the main view's
    {
        EnsureFrontReflection(gfx);
        EnsureNprLighting(gfx);
        EnsureMotionProbe(gfx);
    }
    EnsureDenoiser(gfx, w, h);
    if(w==m_ws->w && h==m_ws->h && m_hist->accum.IsValid())
    {
        if(!rrEnabled && (m_flags&32u) && !m_ws->reference)
        {
            m_ws->reference=std::make_unique<NrdDenoiser>();
            m_ws->reference->SetKind(NrdDenoiser::Kind::Reference);
            if(!m_ws->reference->Resize(gfx,w,h)) LOG_ERROR("PT: NRD reference restore failed");
        }
        return;
    }
    if (m_hist->accum.IsValid()) gfx.DestroyTexture(m_hist->accum);
    m_hist->accum = {};
    RHI::TextureDesc td{};
    td.width = w; td.height = h; td.depth = 1; td.mipLevels = 1;
    td.format = RHI::Format::R32G32B32A32_FLOAT;
    td.usage = RHI::Usage::DEFAULT;
    td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
    td.layout = RHI::ResourceState::UNORDERED_ACCESS;
    if (!gfx.CreateTexture(td, m_hist->accum)) { LOG_ERROR("PathTracePass: accumulation allocation failed"); return; }
    m_hist->accumState = td.layout; m_ws->w = w; m_ws->h = h; ResetHistory();
    if ((m_flags & 32u) && !rrEnabled)
    {
        // A view's segment cannot drain the GPU before NRD resizes its pool: its old instance retires.
        if (!main && m_ws->reference) m_retiredNrd.emplace_back(std::move(m_ws->reference), RHI::kFramesInFlight + 1u);
        if (!m_ws->reference)
        {
            m_ws->reference = std::make_unique<NrdDenoiser>();
            m_ws->reference->SetKind(NrdDenoiser::Kind::Reference);
        }
        if (!m_ws->reference->Resize(gfx, w, h))
            LOG_ERROR("PathTracePass: NRD REFERENCE initialization failed");
    }
}

// The media variants (PT_MEDIA, PathTrace.cs.hlsl): the image's entries and FogCamera, on the
// pass's layout.
bool PathTracePass::BuildMediaPipelines()
{
    if (m_mediaPso.IsValid()) return true;
    if (m_mediaFailed || !m_pso.IsValid() || !m_layout.IsValid() || !m_gfx) return false;
    m_mediaFailed = true;   // cleared once everything is built
    IGraphicsDevice& gfx = *m_gfx;
    const std::vector<DxcCompiler::Define> media = { { "PT_MEDIA", "1" } };
    auto make = [&](const char* entry, const char* tag, RHI::PipelineState& out)
    {
        const auto dxil = ShaderDiskCache::LoadOrCompile("shaders/PathTrace.cs.hlsl", entry, RHI::ShaderStage::CS, media, tag);
        return !dxil.empty() && ComputePipelineBatch::Create(gfx, m_layout, &dxil, 1, &out);
    };
    if (!make(MainEntry(), "PathTraceMedia", m_mediaPso) ||
        ((m_flags & 32768u) && !make(SpatialEntry(), "PathTraceMediaSpatial", m_mediaSpatialPso)) ||
        !make("FogCamera", "PathTraceFog", m_fogPso) || !m_fogCb.Create(gfx, "PT.FogConstants"))
    {
        for (RHI::PipelineState* pso : { &m_mediaPso, &m_mediaSpatialPso, &m_fogPso })
            gfx.DestroyPipelineState(*pso);
        LOG_ERROR("PathTracePass: media pipelines failed");
        return false;
    }
    m_mediaFailed = false;
    return true;
}

// The bound view's camera fog accumulation at its image's size; re-made with it.
bool PathTracePass::EnsureCameraFog(IGraphicsDevice& gfx)
{
    if (!m_ws->w || !m_ws->h) return false;
    if (m_hist->fog.IsValid() && m_hist->fogW == m_ws->w && m_hist->fogH == m_ws->h) return true;
    if (m_hist->fog.IsValid()) ReleaseCameraFog(gfx);
    RHI::TextureDesc td{};
    td.width = m_ws->w; td.height = m_ws->h; td.format = RHI::Format::R32G32B32A32_FLOAT;
    td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
    td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "PT.CameraFog";
    if (!gfx.CreateTexture(td, m_hist->fog)) { LOG_ERROR("PathTracePass: camera fog allocation failed"); return false; }
    m_hist->fogState = td.layout; m_hist->fogW = m_ws->w; m_hist->fogH = m_ws->h; m_hist->fogValid = false;
    return true;
}

void PathTracePass::ReleaseCameraFog(IGraphicsDevice& gfx)
{
    if (m_ws == &m_main) gfx.WaitIdleAndReleaseDeferred();   // a view's segment: the destroys are deferred
    if (m_hist->fog.IsValid()) gfx.DestroyTexture(m_hist->fog);
    m_hist->fog = {}; m_hist->fogState = RHI::ResourceState::UNDEFINED; m_hist->fogValid = false; m_hist->fogW = m_hist->fogH = 0;
    ReleaseFogDenoiser(gfx);
}

void PathTracePass::WriteMediaConstants(Constants& cb) const
{
    cb.mediumIdx = m_ws->fd.mediumIdx;
    cb.mediaFlags = (Settings::Pass::ptMediaSingleScatter() ? 1u : 0u) | (Settings::Pass::ptMediaEquiAngular() ? 2u : 0u);
    cb.lightMediumScaleIdx = m_ws->fd.lightMediumScaleIdx;
    cb.sunMediumScale = m_ws->fd.sunMediumScale;
}

uint32_t PathTracePass::GetCameraFogIndex() const
{
    if (!m_ws->fogWanted || !m_gfx) return 0;
    // The preview denoiser's target on a frame it filters the fog (written later this frame).
    if (m_ws->fogDenoise)
    {
        const uint32_t index = m_gfx->DescriptorHeapIndexFromGpuHandle(m_gfx->GetTextureSRVGpuHandle(m_hist->fogDnOut));
        return index == ~0u ? 0u : index;
    }
    if (!m_hist->fogValid || !m_hist->fog.IsValid()) return 0;
    const uint32_t index = m_gfx->DescriptorHeapIndexFromGpuHandle(m_gfx->GetTextureSRVGpuHandle(m_hist->fog));
    return index == ~0u ? 0u : index;
}

SharcCache::Lookup PathTracePass::GetSharcLookup() const
{
    if (!m_gfx || !m_rtxdi || !UsesRtxdi()) return {};
    return m_rtxdi->GetSharcLookup(*m_gfx);
}

// The camera segment's fog (PathTrace.cs.hlsl FogCamera), after the image: its own constants and a
// running mean that restarts as SetFrameData decided - with the image, whenever the camera moves
// (Area keeps the image's sample count through motion; the fog's mean is the view's) and on a frame
// that does not hold still. imageSpp is the sample index the image used this frame: an image restart
// SetFrameData did not foresee (the object-motion probe, the bounce cap's release) restarts the fog
// now on a frame that filters it or when nothing would, else on the next one, filtered - the
// composite already reads the running mean on this one.
void PathTracePass::RecordCameraFog(RHI::CommandList cl, uint32_t imageSpp)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    if (imageSpp == 0 && !m_ws->fogRestart)
    {
        if (m_ws->fogDenoise || !m_hist->fogDnOut.IsValid() || !DenoiseRuns(1, true))
        { m_ws->fogRestart = true; m_ws->fogSamples = 1; }
        else m_hist->fogStale = true;
    }
    if (m_ws->fogRestart) m_hist->fogSpp = 0;
    Constants cb = AuxConstants(m_ws->w, m_ws->h);
    cb.spp = m_hist->fogSpp;
    cb.flags |= m_flags & 4096u;   // -pt_single_sample: no running mean
    cb.seed = (m_hist->fogFrame++) ^ (m_seed * 0x9e3779b9u);
    cb.depthIdx = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(m_depth)));
    WriteMediaConstants(cb);
    if (m_hist->fogState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->fog, m_hist->fogState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_hist->fogState = RHI::ResourceState::UNORDERED_ACCESS;
    cb.auxOutUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->fog));
    *m_fogCb.Current(gfx) = cb;
    BindAuxRoot(gfx, cl, m_fogCb.CurrentBuffer(gfx), m_fogPso);
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.Fog");
    gfx.DispatchCompute((m_ws->w + 7) / 8, (m_ws->h + 7) / 8, 1, cl);
    gfx.EndGPUTimestamp(cl, timer);
    // PathTraceFogPass reads it later in this graph (bindless).
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->fog, m_hist->fogState, RHI::ResourceState::SHADER_RESOURCE), cl);
    m_hist->fogState = RHI::ResourceState::SHADER_RESOURCE;
    m_hist->fogValid = true;
    m_hist->fogViewProj = m_ws->fd.viewProj;
    m_hist->fogSpp = (m_ws->fd.stable && !(m_flags & 4096u)) ? m_hist->fogSpp + 1 : 0;
    // As the image's [PT] line: the first sample and every 30th.
    if (m_ws == &m_main && (m_hist->fogSpp <= 1 || m_hist->fogSpp % 30 == 0))
        LOG_INFO("[PT_FOG] spp=%u imageSpp=%u stable=%u width=%u height=%u", m_hist->fogSpp, imageSpp,
                 m_ws->fd.stable ? 1u : 0u, m_ws->w, m_ws->h);
}

// The front-layer reflection's entry, constants and accumulation (an RGBA32F running mean,
// as the image's): built the first frame it is wanted, the texture re-made with the render
// size, and its denoiser (EnsureFrontDenoiser) with it. Every frame that does not accumulate
// denoises, RR or not: a scene that never holds still (an animated sky, skinned characters)
// would otherwise show a raw sample on the panes every frame.
void PathTracePass::EnsureFrontReflection(IGraphicsDevice& gfx)
{
    if (!m_frontWanted || !m_frontDepth.IsValid() || m_frontFailed || !m_layout.IsValid()) return;
    if (!m_frontPso.IsValid())
    {
        auto shader = ShaderDiskCache::LoadOrCompile("shaders/PathTrace.cs.hlsl", "FrontReflection", RHI::ShaderStage::CS, {}, "PathTraceFront");
        if (shader.empty() || !ComputePipelineBatch::Create(gfx, m_layout, &shader, 1, &m_frontPso) ||
            !m_frontCb.Create(gfx, "PT.FrontConstants"))
        {
            gfx.DestroyPipelineState(m_frontPso); m_frontFailed = true;
            LOG_ERROR("PathTracePass: front-layer reflection pipeline failed");
            return;
        }
    }
    const auto w = gfx.GetRenderWidth(), h = gfx.GetRenderHeight();
    if (!w || !h) return;
    if (!m_front.IsValid() || w != m_frontW || h != m_frontH)
    {
        if (m_front.IsValid()) ReleaseFrontReflection(gfx);
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.format = RHI::Format::R32G32B32A32_FLOAT;
        td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = "PT.FrontReflection";
        if (!gfx.CreateTexture(td, m_front))
        { m_frontFailed = true; LOG_ERROR("PathTracePass: front-layer reflection allocation failed"); return; }
        m_frontState = td.layout; m_frontW = w; m_frontH = h; m_frontValid = false;
        LOG_INFO("[PT_FRONT] allocated=1 width=%u height=%u bytes=%llu", w, h, static_cast<unsigned long long>(w) * h * 16);
    }
    m_frontMirrorSamples = static_cast<uint32_t>(std::clamp(Settings::Pass::ptFrontSpp(), 1, 8));
    // The local lights at the reflection's vertices: this many candidates by power, resampled by
    // their unshadowed contribution, one shadow ray (PathTrace.cs.hlsl RTLocalLightsRIS); 1 = one
    // light by power.
    m_frontNeeCandidates = static_cast<uint32_t>(std::clamp(Settings::Pass::ptFrontNee(), 1, 32));
    EnsureFrontDenoiser(gfx, w, h);
}

void PathTracePass::ReleaseFrontReflection(IGraphicsDevice& gfx)
{
    if (m_frontNrd) ReleaseFrontDenoiser(gfx);
    gfx.WaitIdleAndReleaseDeferred();
    gfx.DestroyTexture(m_front);
    m_front = {}; m_frontState = RHI::ResourceState::UNDEFINED; m_frontValid = false;
    LOG_INFO("[PT_FRONT] released=1");
}

// The front reflection's denoiser (PathTraceFrontNRD.cs.hlsl + REBLUR_SPECULAR): the
// per-frame sample, NRD's four inputs and its output at the front texture's size; released
// with the front texture.
bool PathTracePass::EnsureFrontDenoiser(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (m_frontNrd && m_frontNrd->IsReady() && m_frontNoisy.IsValid()) return true;
    if (!m_frontPackPso.IsValid() || !m_frontResolvePso.IsValid())
    {
        auto pack = ShaderDiskCache::LoadOrCompile("shaders/PathTraceFrontNRD.cs.hlsl", "Pack", RHI::ShaderStage::CS, {}, "PathTraceFrontNRD");
        auto resolve = ShaderDiskCache::LoadOrCompile("shaders/PathTraceFrontNRD.cs.hlsl", "Resolve", RHI::ShaderStage::CS, {}, "PathTraceFrontNRD");
        bool ok = !pack.empty() && !resolve.empty();
        if (ok) ok = ComputePipelineBatch::Create(gfx, m_layout, &pack, 1, &m_frontPackPso);
        if (ok) ok = ComputePipelineBatch::Create(gfx, m_layout, &resolve, 1, &m_frontResolvePso);
        if (ok && !m_frontNrdCb.IsValid()) ok = m_frontNrdCb.Create(gfx, "PT.FrontNrdConstants");
        if (!ok)
        {
            gfx.DestroyPipelineState(m_frontPackPso); gfx.DestroyPipelineState(m_frontResolvePso); m_frontFailed = true;
            LOG_ERROR("PathTracePass: front-layer reflection denoiser pipeline failed");
            return false;
        }
    }
    const struct { RHI::Format format; RHI::Texture* texture; const char* name; } targets[6] = {
        { RHI::Format::R16G16B16A16_FLOAT, &m_frontNoisy,          "PT.FrontNoisy" },
        { RHI::Format::R10G10B10A2_UNORM,  &m_frontNrdNormalRough, "PT.FrontNrdNormalRough" },
        { RHI::Format::R32_FLOAT,          &m_frontNrdViewZ,       "PT.FrontNrdViewZ" },
        { RHI::Format::R16G16_FLOAT,       &m_frontNrdMV,          "PT.FrontNrdMV" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_frontNrdSpecIn,      "PT.FrontNrdSpecIn" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_frontNrdSpecOut,     "PT.FrontNrdSpecOut" },
    };
    for (uint32_t i = 0; i < 6; ++i)
    {
        if (targets[i].texture->IsValid()) continue;
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.format = targets[i].format;
        td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = targets[i].name;
        if (!gfx.CreateTexture(td, *targets[i].texture))
        { m_frontFailed = true; LOG_ERROR("PathTracePass: front-layer reflection denoiser allocation failed"); return false; }
        m_frontNrdStates[i] = td.layout;
    }
    if (!m_frontNrd)
    {
        m_frontNrd = std::make_unique<NrdDenoiser>();
        m_frontNrd->SetKind(NrdDenoiser::Kind::ReblurSpecularPathTraced);
    }
    if (!m_frontNrd->Resize(gfx, w, h))
    { m_frontFailed = true; LOG_ERROR("PathTracePass: front-layer reflection NRD initialization failed"); return false; }
    m_frontNrdValid = false;
    LOG_INFO("[PT_FRONT] denoiser=REBLUR_SPECULAR allocated=1 width=%u height=%u", w, h);
    return true;
}

void PathTracePass::ReleaseFrontDenoiser(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred();
    for (RHI::Texture* t : { &m_frontNoisy, &m_frontNrdNormalRough, &m_frontNrdViewZ, &m_frontNrdMV, &m_frontNrdSpecIn, &m_frontNrdSpecOut })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_frontNrd.reset();
    m_frontNrdValid = false;
    LOG_INFO("[PT_FRONT] denoiser released=1");
}

// PT-lit toon receivers: the sample entry, the denoiser's entries, their
// constants and the render-size textures, built the first frame they are
// wanted and re-made with the render size.
void PathTracePass::EnsureNprLighting(IGraphicsDevice& gfx)
{
    if (!m_nprWanted || m_nprFailed || !m_layout.IsValid()) return;
    if (!m_nprPso.IsValid() || !m_nprTemporalPso.IsValid() || !m_nprSpatialPso.IsValid())
    {
        auto make = [&](const char* file, const char* entry, RHI::PipelineState& pso)
        {
            auto shader = ShaderDiskCache::LoadOrCompile(file, entry, RHI::ShaderStage::CS, {}, "PathTraceNpr");
            return !shader.empty() && ComputePipelineBatch::Create(gfx, m_layout, &shader, 1, &pso);
        };
        bool ok = make("shaders/PathTrace.cs.hlsl", "NprAmbient", m_nprPso) &&
                  make("shaders/PathTraceNprDenoise.cs.hlsl", "Temporal", m_nprTemporalPso) &&
                  make("shaders/PathTraceNprDenoise.cs.hlsl", "Spatial", m_nprSpatialPso);
        if (ok && !m_nprCb.IsValid()) ok = m_nprCb.Create(gfx, "PT.NprConstants");
        for (auto& cb : m_nprDenoiseCb)
            if (ok && !cb.IsValid()) ok = cb.Create(gfx, "PT.NprDenoiseConstants");
        if (!ok)
        {
            gfx.DestroyPipelineState(m_nprPso); gfx.DestroyPipelineState(m_nprTemporalPso); gfx.DestroyPipelineState(m_nprSpatialPso);
            m_nprFailed = true;
            LOG_ERROR("PathTracePass: toon receiver lighting pipeline failed");
            return;
        }
    }
    const auto w = gfx.GetRenderWidth(), h = gfx.GetRenderHeight();
    if (!w || !h || (m_nprFinal.IsValid() && w == m_nprW && h == m_nprH)) return;
    if (m_nprFinal.IsValid()) ReleaseNprLighting(gfx);
    const struct { RHI::Format format; RHI::Texture* texture; const char* name; } targets[7] = {
        { RHI::Format::R16G16B16A16_FLOAT, &m_nprNoisy,      "PT.NprNoisy" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->nprHistory[0], "PT.NprHistory" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->nprHistory[1], "PT.NprHistory" },
        { RHI::Format::R32G32_FLOAT,       &m_hist->nprMeta[0],    "PT.NprHistoryMeta" },
        { RHI::Format::R32G32_FLOAT,       &m_hist->nprMeta[1],    "PT.NprHistoryMeta" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_nprSpatial,    "PT.NprSpatial" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_nprFinal,      "PT.NprLighting" },
    };
    for (uint32_t i = 0; i < 7; ++i)
    {
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.format = targets[i].format;
        td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = targets[i].name;
        if (!gfx.CreateTexture(td, *targets[i].texture))
        { m_nprFailed = true; LOG_ERROR("PathTracePass: toon receiver lighting allocation failed"); return; }
        NprState(i) = td.layout;
    }
    m_nprW = w; m_nprH = h; m_hist->nprWrite = 0; m_hist->nprValid = false;
    LOG_INFO("[PT_NPR] allocated=1 width=%u height=%u", w, h);
}

void PathTracePass::ReleaseNprLighting(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred();
    for (RHI::Texture* t : { &m_nprNoisy, &m_hist->nprHistory[0], &m_hist->nprHistory[1], &m_hist->nprMeta[0], &m_hist->nprMeta[1], &m_nprSpatial, &m_nprFinal })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->nprValid = false;
    LOG_INFO("[PT_NPR] released=1");
}

// The preview denoiser's entries, constants and render-size textures, built the first frame it
// is wanted and re-made with the render size.
void PathTracePass::EnsureDenoiser(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
{
    if (!m_dnWanted && m_hist->dnSampleMoment.IsValid()) ReleaseDenoiser(gfx);   // a view's (SetDenoise frees the main view's)
    if (!m_dnWanted || m_dnFailed || !m_layout.IsValid()) return;
    if (!m_dnTemporalPso.IsValid() || !m_dnAtrousPso.IsValid())
    {
        auto make = [&](const char* entry, RHI::PipelineState& pso)
        {
            auto shader = ShaderDiskCache::LoadOrCompile("shaders/PathTraceDenoise.cs.hlsl", entry, RHI::ShaderStage::CS, {}, "PathTraceDenoise");
            return !shader.empty() && ComputePipelineBatch::Create(gfx, m_layout, &shader, 1, &pso);
        };
        bool ok = make("Temporal", m_dnTemporalPso) && make("Atrous", m_dnAtrousPso);
        for (auto& cb : m_dnCb)
            if (ok && !cb.IsValid()) ok = cb.Create(gfx, "PT.DenoiseConstants");
        if (!ok)
        {
            gfx.DestroyPipelineState(m_dnTemporalPso); gfx.DestroyPipelineState(m_dnAtrousPso);
            m_dnFailed = true;
            LOG_ERROR("PathTracePass: preview denoiser pipeline failed");
            return;
        }
    }
    if (!w || !h || (m_hist->dnSampleMoment.IsValid() && w == m_hist->dnW && h == m_hist->dnH)) return;
    if (m_hist->dnSampleMoment.IsValid()) ReleaseDenoiser(gfx);
    const struct { RHI::Format format; RHI::Texture* texture; const char* name; } targets[9] = {
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnHistory[0], "PT.DenoiseHistory" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnHistory[1], "PT.DenoiseHistory" },
        { RHI::Format::R32G32_FLOAT,       &m_hist->dnMoments[0], "PT.DenoiseMoments" },
        { RHI::Format::R32G32_FLOAT,       &m_hist->dnMoments[1], "PT.DenoiseMoments" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnMeta[0],    "PT.DenoiseMeta" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnMeta[1],    "PT.DenoiseMeta" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnFilter[0],  "PT.DenoiseFilter" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->dnFilter[1],  "PT.DenoiseFilter" },
        { RHI::Format::R32_FLOAT,          &m_hist->dnSampleMoment, "PT.DenoiseSampleMoment" },
    };
    uint64_t bytes = 0;
    for (const auto& t : targets)
    {
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.format = t.format;
        td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = t.name;
        if (!gfx.CreateTexture(td, *t.texture))
        { m_dnFailed = true; ReleaseDenoiser(gfx); LOG_ERROR("PathTracePass: preview denoiser allocation failed"); return; }
        bytes += uint64_t(w) * h * (t.format == RHI::Format::R32_FLOAT ? 4u : 8u);
    }
    m_hist->dnW = w; m_hist->dnH = h; m_hist->dnWrite = 0; m_hist->dnHistoryValid = false; m_hist->dnMomentValid = false;
    if (m_ws == &m_main)
        LOG_INFO("[PT_DENOISE] allocated=1 width=%u height=%u bytes=%llu", w, h, static_cast<unsigned long long>(bytes));
}

// The image's working set and the fog's: the fog's filter borrows the image's a-trous ping-pong.
void PathTracePass::ReleaseDenoiser(IGraphicsDevice& gfx)
{
    const bool main = m_ws == &m_main;
    if (main) gfx.WaitIdleAndReleaseDeferred();   // a view's segment: the destroys are deferred
    for (RHI::Texture* t : { &m_hist->dnHistory[0], &m_hist->dnHistory[1], &m_hist->dnMoments[0], &m_hist->dnMoments[1], &m_hist->dnMeta[0],
                             &m_hist->dnMeta[1], &m_hist->dnFilter[0], &m_hist->dnFilter[1], &m_hist->dnSampleMoment })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->dnHistoryValid = false;
    ReleaseFogDenoiser(gfx);
    if (main) LOG_INFO("[PT_DENOISE] released=1");
}

// The camera fog's filter: its entries and constants on first use (for every view), the bound view's
// targets at its image's size.
void PathTracePass::EnsureFogDenoiser(IGraphicsDevice& gfx)
{
    if (!m_dnWanted || m_fogDnFailed || !m_layout.IsValid()) return;
    if (!m_fogDnTemporalPso.IsValid() || !m_fogDnAtrousPso.IsValid())
    {
        auto make = [&](const char* entry, RHI::PipelineState& pso)
        {
            auto shader = ShaderDiskCache::LoadOrCompile("shaders/PathTraceDenoise.cs.hlsl", entry, RHI::ShaderStage::CS, {}, "PathTraceDenoise");
            return !shader.empty() && ComputePipelineBatch::Create(gfx, m_layout, &shader, 1, &pso);
        };
        bool ok = make("FogTemporal", m_fogDnTemporalPso) && make("FogAtrous", m_fogDnAtrousPso);
        for (auto& cb : m_fogDnCb)
            if (ok && !cb.IsValid()) ok = cb.Create(gfx, "PT.FogDenoiseConstants");
        if (!ok)
        {
            gfx.DestroyPipelineState(m_fogDnTemporalPso); gfx.DestroyPipelineState(m_fogDnAtrousPso);
            m_fogDnFailed = true;
            LOG_ERROR("PathTracePass: camera fog denoiser pipeline failed");
            return;
        }
    }
    const uint32_t w = m_ws->w, h = m_ws->h;
    if (!w || !h || (m_hist->fogDnOut.IsValid() && m_hist->fogDnW == w && m_hist->fogDnH == h)) return;
    if (m_hist->fogDnOut.IsValid()) ReleaseFogDenoiser(gfx);
    const struct { RHI::Format format; RHI::Texture* texture; const char* name; } targets[5] = {
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->fogDnHistory[0], "PT.FogDenoiseHistory" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->fogDnHistory[1], "PT.FogDenoiseHistory" },
        { RHI::Format::R16G16_FLOAT,       &m_hist->fogDnMeta[0],    "PT.FogDenoiseMeta" },
        { RHI::Format::R16G16_FLOAT,       &m_hist->fogDnMeta[1],    "PT.FogDenoiseMeta" },
        { RHI::Format::R16G16B16A16_FLOAT, &m_hist->fogDnOut,        "PT.FogDenoised" },
    };
    uint64_t bytes = 0;
    for (const auto& t : targets)
    {
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.format = t.format;
        td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        td.usage = RHI::Usage::DEFAULT; td.layout = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = t.name;
        if (!gfx.CreateTexture(td, *t.texture))
        { m_fogDnFailed = true; ReleaseFogDenoiser(gfx); LOG_ERROR("PathTracePass: camera fog denoiser allocation failed"); return; }
        bytes += uint64_t(w) * h * (t.format == RHI::Format::R16G16_FLOAT ? 4u : 8u);
    }
    m_hist->fogDnOutState = RHI::ResourceState::UNORDERED_ACCESS;
    m_hist->fogDnW = w; m_hist->fogDnH = h; m_hist->fogDnWrite = 0; m_hist->fogDnHistoryValid = false;
    if (m_ws == &m_main)
        LOG_INFO("[PT_FOG_DENOISE] allocated=1 width=%u height=%u bytes=%llu", w, h, static_cast<unsigned long long>(bytes));
}

void PathTracePass::ReleaseFogDenoiser(IGraphicsDevice& gfx)
{
    RHI::Texture* const targets[5] = { &m_hist->fogDnHistory[0], &m_hist->fogDnHistory[1], &m_hist->fogDnMeta[0],
                                       &m_hist->fogDnMeta[1], &m_hist->fogDnOut };
    bool any = false;
    for (const RHI::Texture* t : targets) any = any || t->IsValid();
    m_hist->fogDnW = m_hist->fogDnH = 0; m_hist->fogDnHistoryValid = false; m_ws->fogDenoise = false;
    if (!any) return;
    const bool main = m_ws == &m_main;
    if (main) gfx.WaitIdleAndReleaseDeferred();   // a view's segment: the destroys are deferred
    for (RHI::Texture* t : targets) { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->fogDnOutState = RHI::ResourceState::UNDEFINED;
    if (main) LOG_INFO("[PT_FOG_DENOISE] released=1");
}

void PathTracePass::Shutdown(IGraphicsDevice& gfx)
{
    for (RHI::Texture* t : { &m_hist->dnHistory[0], &m_hist->dnHistory[1], &m_hist->dnMoments[0], &m_hist->dnMoments[1], &m_hist->dnMeta[0],
                             &m_hist->dnMeta[1], &m_hist->dnFilter[0], &m_hist->dnFilter[1], &m_hist->dnSampleMoment })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->dnHistoryValid = false;
    gfx.DestroyPipelineState(m_dnTemporalPso); gfx.DestroyPipelineState(m_dnAtrousPso);
    for (auto& cb : m_dnCb) cb.Destroy(gfx);
    for (RHI::Texture* t : { &m_hist->fogDnHistory[0], &m_hist->fogDnHistory[1], &m_hist->fogDnMeta[0], &m_hist->fogDnMeta[1],
                             &m_hist->fogDnOut })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->fogDnOutState = RHI::ResourceState::UNDEFINED;
    m_hist->fogDnW = m_hist->fogDnH = 0; m_hist->fogDnHistoryValid = false; m_ws->fogDenoise = false;
    gfx.DestroyPipelineState(m_fogDnTemporalPso); gfx.DestroyPipelineState(m_fogDnAtrousPso);
    for (auto& cb : m_fogDnCb) cb.Destroy(gfx);
    for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
    {
        if (m_motionMapped[i]) gfx.UnmapBuffer(m_motionReadback[i]);
        if (m_motionReadback[i].IsValid()) gfx.DestroyBuffer(m_motionReadback[i]);
        m_motionReadback[i] = {}; m_motionMapped[i] = nullptr; m_motionPrimed[i] = false;
    }
    if (m_motion.IsValid()) gfx.DestroyBuffer(m_motion);
    m_motion = {}; m_motionState = RHI::ResourceState::UNDEFINED;
    gfx.DestroyPipelineState(m_motionClearPso); gfx.DestroyPipelineState(m_motionProbePso);
    m_motionCb.Destroy(gfx);
    if(m_ws->areaRadiance.IsValid()) gfx.DestroyTexture(m_ws->areaRadiance);
    m_ws->areaRadiance={}; m_ws->areaRadianceState=RHI::ResourceState::UNDEFINED;
    for(auto& history:m_hist->areaHistory) { if(history.IsValid()) gfx.DestroyTexture(history); history={}; }
    for(auto& film:m_hist->areaFilm) { if(film.IsValid()) gfx.DestroyTexture(film); film={}; }
    for(auto& t:m_hist->areaPrimary) { if(t.IsValid()) gfx.DestroyTexture(t); t={}; }
    for(auto& t:m_hist->areaGeometry) { if(t.IsValid()) gfx.DestroyTexture(t); t={}; }
    for(auto& t:m_hist->areaLens) { if(t.IsValid()) gfx.DestroyTexture(t); t={}; }
    for(auto& guide:m_rrGuides) { if(guide.IsValid()) gfx.DestroyTexture(guide); guide={}; }
    if (m_front.IsValid()) gfx.DestroyTexture(m_front);
    m_front = {}; m_frontState = RHI::ResourceState::UNDEFINED; m_frontValid = false;
    gfx.DestroyPipelineState(m_frontPso); m_frontCb.Destroy(gfx);
    if (m_hist->fog.IsValid()) gfx.DestroyTexture(m_hist->fog);
    m_hist->fog = {}; m_hist->fogState = RHI::ResourceState::UNDEFINED; m_hist->fogValid = false; m_hist->fogW = m_hist->fogH = 0;
    for (RHI::PipelineState* pso : { &m_mediaPso, &m_mediaSpatialPso, &m_fogPso })
        gfx.DestroyPipelineState(*pso);
    m_fogCb.Destroy(gfx);
    for (RHI::Texture* t : { &m_frontNoisy, &m_frontNrdNormalRough, &m_frontNrdViewZ, &m_frontNrdMV, &m_frontNrdSpecIn, &m_frontNrdSpecOut })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_frontNrd.reset(); m_frontNrdValid = false;
    gfx.DestroyPipelineState(m_frontPackPso); gfx.DestroyPipelineState(m_frontResolvePso); m_frontNrdCb.Destroy(gfx);
    for (RHI::Texture* t : { &m_nprNoisy, &m_hist->nprHistory[0], &m_hist->nprHistory[1], &m_hist->nprMeta[0], &m_hist->nprMeta[1], &m_nprSpatial, &m_nprFinal })
    { if (t->IsValid()) gfx.DestroyTexture(*t); *t = {}; }
    m_hist->nprValid = false;
    gfx.DestroyPipelineState(m_nprPso); gfx.DestroyPipelineState(m_nprTemporalPso); gfx.DestroyPipelineState(m_nprSpatialPso);
    m_nprCb.Destroy(gfx);
    for (auto& cb : m_nprDenoiseCb) cb.Destroy(gfx);
    if (m_hist->accum.IsValid()) gfx.DestroyTexture(m_hist->accum);
    for (uint32_t i=0; i<RHI::kFramesInFlight; ++i)
    {
        if (m_mapped[i]) gfx.UnmapBuffer(m_readback[i]);
        if (m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
        m_readback[i] = {}; m_mapped[i] = nullptr; m_primed[i] = false;
    }
    if (m_stats.IsValid()) gfx.DestroyBuffer(m_stats);
    m_stats = {}; gfx.DestroyPipelineState(m_clearStats);
    gfx.DestroyPipelineState(m_probe); gfx.DestroyPipelineState(m_clearProbe); m_probeDispatched = m_probeReported = false;
    gfx.DestroyPipelineState(m_dielectricProbe); gfx.DestroyPipelineState(m_dielectricProbeClear);
    gfx.DestroyPipelineState(m_dielectricProbeReduce);
    for (RHI::PipelineState* pso : { &m_mediaProbeClear, &m_mediaProbeHg, &m_mediaProbeFreeFlight,
                                     &m_mediaProbeEquiAngular, &m_mediaProbeReduce })
        gfx.DestroyPipelineState(*pso);
    gfx.DestroyPipelineState(m_hitProbe); m_hitProbeReported = false; m_lensConeReported=false; m_reconnectProbeReported=false;
    m_ws->reference.reset();
    m_retiredNrd.clear();   // after the GPU drain
    if (m_rtxdi) { m_rtxdi->Release(gfx); m_rtxdi->ReleasePipelines(gfx); }
    m_hist->accum = {}; m_cb.Destroy(gfx); m_areaSpatialCb.Destroy(gfx); gfx.DestroyPipelineState(m_areaSpatialPso);
    gfx.DestroyPipelineState(m_pso); gfx.DestroyPipelineLayout(m_layout); m_ws->active = false;
}

void PathTracePass::ReloadShaders(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred();
    Shutdown(gfx);
    Init(gfx);
    if (m_wanted) BuildPipelines();   // in use: rebuilt now, not a frame late
    OnCompile(gfx);
    if (m_wanted && UsesRtxdi() && m_rtxdi && m_rtxdi->BuildPipelines(gfx) && m_ws->w && m_ws->h)
        m_rtxdi->Allocate(gfx, m_ws->w, m_ws->h);
    ResetHistory();
    // An Area mode change rebuilds through here: report the new working set.
    m_status.residentBytes = AreaResidentBytes();
    m_logBackend = true;
}

const char* PathTracePass::GetInertReason() const
{
    if (!m_pso.IsValid() || !m_cb.IsValid()) return "pipeline";
    if ((m_flags&32768u) && (!m_areaSpatialCb.IsValid() || !m_areaSpatialPso.IsValid())) return "Area spatial pipeline";
    if (!m_hist->accum.IsValid()) return "accumulation";
    if(m_flags&16384u) for(const auto& history:m_hist->areaHistory) if(!history.IsValid()) return "Area reservoir";
    if(m_flags&4194304u) for(uint32_t i=0;i<2;++i) if(!m_hist->areaPrimary[i].IsValid() || !m_hist->areaGeometry[i].IsValid()) return "Area primary context";
    if(m_flags&65536u) for(const auto& film:m_hist->areaFilm) if(!film.IsValid()) return "Area film rotation";
    if(m_areaV2) for(const auto& lens:m_hist->areaLens) if(!lens.IsValid()) return "Area lens";
    // Ray reconstruction and the RTXDI mode are the main view's.
    const bool main = m_ws == &m_main;
    const bool rrEnabled = main && m_rrEnabled;
    if(rrEnabled) for(const auto& guide:m_rrGuides) if(!guide.IsValid()) return "RR guides";
    if(UsesRtxdi() && (!main || !m_rtxdi || !m_rtxdi->HasPipelines() || !m_rtxdi->IsAllocated())) return "rtxdi core";
    if (!rrEnabled && (m_flags & 32u) && (!m_ws->reference || !m_ws->reference->IsReady())) return "reference";
    if((m_flags&1073741824u) && !m_ws->areaRadiance.IsValid()) return "Area selected radiance";
    const auto* output = m_graph.GetPhysicalTexture(m_result);
    if (!output || !output->IsValid()) return "result";
    return nullptr;
}

uint64_t PathTracePass::GetUpcomingOutputSrv() const
{
    if (GetInertReason()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(m_result));
}

bool PathTracePass::MediaEnabled() const
{
    return Settings::Pass::ptMedia() && !UsesRtxdi();
}

bool PathTracePass::RtxdiMediaEnabled() const
{
    return Settings::Pass::ptRtxdiMedia() && UsesRtxdi();
}

uint32_t PathTracePass::RtxdiMediaRule()
{
    const std::string rule = Settings::Pass::ptRtxdiMediaContinuation();
    return rule == "none" ? 1u : rule == "full" ? 2u : rule == "froxel" ? 3u : 0u;
}

void PathTracePass::SetFrameData(const FrameData& fd, bool active)
{
    // Scene changes invalidate both domains. Camera-only motion can retain the
    // random-stream reservoir, but must never retain progressive image samples.
    if (!active && !m_ws->active) { m_ws->fd = fd; return; }
    const bool sceneChanged = !fd.stable || fd.fingerprint != m_ws->fd.fingerprint ||
        fd.shadingFingerprint != m_ws->fd.shadingFingerprint || fd.lightsFingerprint != m_ws->fd.lightsFingerprint ||
        fd.mediumFingerprint != m_ws->fd.mediumFingerprint ||
        fd.textureRevision != m_ws->fd.textureRevision || fd.surfaceCacheRevision != m_ws->fd.surfaceCacheRevision ||
        fd.environmentScale != m_ws->fd.environmentScale || fd.skyIdx != m_ws->fd.skyIdx ||
        std::memcmp(&fd.sunDirection, &m_ws->fd.sunDirection, sizeof(fd.sunDirection)) != 0 ||
        std::memcmp(&fd.sunColor, &m_ws->fd.sunColor, sizeof(fd.sunColor)) != 0;
    if (active && m_ws->active && sceneChanged && StabilityLog())
        LOG_INFO("[PT_STABILITY] stable=%u tlas=%u shading=%u lights=%u medium=%u texture=%u surfaceCache=%u env=%u sky=%u sun=%u",
            fd.stable?1u:0u, fd.fingerprint!=m_ws->fd.fingerprint?1u:0u, fd.shadingFingerprint!=m_ws->fd.shadingFingerprint?1u:0u,
            fd.lightsFingerprint!=m_ws->fd.lightsFingerprint?1u:0u, fd.mediumFingerprint!=m_ws->fd.mediumFingerprint?1u:0u,
            fd.textureRevision!=m_ws->fd.textureRevision?1u:0u,
            fd.surfaceCacheRevision!=m_ws->fd.surfaceCacheRevision?1u:0u, fd.environmentScale!=m_ws->fd.environmentScale?1u:0u,
            fd.skyIdx!=m_ws->fd.skyIdx?1u:0u,
            (std::memcmp(&fd.sunDirection,&m_ws->fd.sunDirection,sizeof(fd.sunDirection))!=0 ||
             std::memcmp(&fd.sunColor,&m_ws->fd.sunColor,sizeof(fd.sunColor))!=0)?1u:0u);
    m_hist->contentStreak = (active && m_ws->active && sceneChanged) ? m_hist->contentStreak + 1 : 0;
    m_ws->contentChanged = active && m_ws->active && sceneChanged;
    m_ws->cameraMoved = std::memcmp(&fd.viewProj, &m_ws->fd.viewProj, sizeof(fd.viewProj)) != 0;
    if (!active || !m_ws->active)
    {
        ResetHistory();
        m_hist->dnHistoryValid = false;
    }
    // A moving character or animated TLAS content changes the scene every frame.
    // v2 keeps its reservoirs through it: every reused sample is re-traced in
    // the current scene and its pairwise MIS compares the stored target with
    // the shifted one, so only the progressive image must restart.
    else if (sceneChanged)
    {
        // v2 and the rtxdi mode re-evaluate every reused sample in the current scene: only the
        // progressive image restarts.
        if (m_areaV2 || UsesRtxdi()) ResetImage();
        else ResetHistory();
    }
    else if(std::memcmp(&fd.viewProj,&m_ws->fd.viewProj,sizeof(fd.viewProj))!=0)
    {
        if(UsesRtxdi()) ResetImage();
        else if(!m_areaMotion) ResetHistory();
        else
        {
            m_hist->spp=0;
            if(!m_hist->resetPending) ++m_hist->resets;
            m_hist->resetPending=true;
            LOG_INFO("[AREA_MOTION] retained=%u sequence=%u",m_hist->areaValid?1u:0u,m_hist->areaFrame);
        }
    }
    m_ws->fd = fd; m_ws->active = active;
    // The camera segment's fog: traced on the GBuffer primary while the medium is (the camera
    // primary integrates it in the path). Its pipelines and target are made here, before the graph
    // records: the composite reads GetCameraFogIndex then.
    m_ws->fogWanted = active && fd.mediumIdx != 0 && !UsesCameraPrimary() && !UsesRtxdi() && m_gfx &&
                  BuildMediaPipelines() && EnsureCameraFog(*m_gfx);
    // This frame's camera fog, decided before the graph records (GetCameraFogIndex hands the
    // composite its target then): whether the running mean restarts, its samples after the frame
    // and whether the preview denoiser filters it.
    if (m_ws->fogWanted) EnsureFogDenoiser(*m_gfx);
    else m_hist->fogDnHistoryValid = false;
    m_ws->fogRestart = m_hist->fogStale || !m_hist->fogValid || !fd.stable || m_hist->spp == 0 ||
                       std::memcmp(&m_hist->fogViewProj, &fd.viewProj, sizeof(m_hist->fogViewProj)) != 0;
    m_hist->fogStale = false;
    m_ws->fogSamples = (m_ws->fogRestart || (m_flags & 4096u)) ? 1u : m_hist->fogSpp + 1u;
    m_ws->fogDenoise = m_ws->fogWanted && m_hist->fogDnOut.IsValid() && DenoiseRuns(m_ws->fogSamples, m_ws->fogRestart);
    // The rtxdi core's media builds, the same way (Record uses them once they exist).
    if (active && fd.rtxdiMediumIdx != 0 && UsesRtxdi() && m_rtxdi && m_gfx && m_rtxdi->HasPipelines())
        m_rtxdi->BuildMediaPipelines(*m_gfx);
}

RHI::CommandList PathTracePass::Execute(RHI::CommandList cl)
{
    if (!m_ws->active || GetInertReason() || !m_ws->fd.tlasVA || !m_ws->fd.instanceVA || !m_ws->fd.buffers)
    {
        m_hist->dnHistoryValid = false;   // the next denoised frame must not reproject across the gap
        m_hist->fogDnHistoryValid = false;
        return cl;
    }
    IGraphicsDevice& gfx = cl.GetDevice();
    // The stats, the probes (the motion probe with them), the RR guides, the front reflection, the toon
    // lighting and the RTXDI mode are the main view's.
    const bool main = m_ws == &m_main;
    const bool rrEnabled = main && m_rrEnabled;
    // A secondary view draws the main view's random stream of this frame (the main view recorded first):
    // through the same camera it is the same image, and the views' noise is the same.
    if (!main && m_mainHist) m_hist->rngFrame = m_mainHist->rngFrame - 1u;
    const auto slot = gfx.GetFrameIndex();
    // Geometry the fingerprints do not see moved on screen (a still-camera frame's motion vectors,
    // read back now): a content change, as SetFrameData would have counted it - unless
    // -pt_static_scene accumulates through whatever moves. A secondary view takes the main view's
    // verdict of this frame.
    if (main) m_objectMotion = ReadObjectMotion(gfx);
    if (m_objectMotion && !m_ws->fd.staticScene && !m_ws->contentChanged)
    {
        ++m_hist->contentStreak;
        m_ws->contentChanged = true;
        if (m_areaV2 || UsesRtxdi()) ResetImage();
        else ResetHistory();
    }
    // The scene moves (its content changed this frame and the one before, or it is unstable; a
    // single change, or the camera alone, restarts and accumulates on at full length): such a frame
    // gains nothing from unlimited bounces and pays for the longest path in each wave, so the
    // settings' unlimited arm becomes 4 bounces with roulette from the 2nd. Explicit -pt_bounces /
    // -pt_reference stay exact. The first frame back at full bounces restarts the image and the
    // denoiser's history, so no capped sample stays in either.
    const bool moving = !m_ws->fd.stable || m_hist->contentStreak >= 2;
    const bool capped = (moving || (m_flags & 16384u)) && m_referenceFromSettings && !m_bounceCapCli;
    if (!UsesRtxdi() && !capped && m_hist->lastCapped)
    {
        if (m_hist->spp != 0) ResetImage();
        m_hist->dnHistoryValid = false;
    }
    const uint32_t imageSpp = m_hist->spp;   // this frame's sample index; both cores advance it
    // The preview denoiser, after either core's trace: the image's samples after this frame (a
    // frame that does not accumulate keeps one). A frame skipped because the image has faded into
    // the running mean keeps the history (the scene has held still since); any other skip drops it.
    auto denoise = [&]
    {
        if (main) RecordMotionProbe(cl);
        const uint32_t samples = m_ws->fd.stable ? imageSpp + 1u : 1u;
        if (DenoiseRuns(samples, imageSpp == 0)) RecordDenoise(cl, samples, imageSpp == 0);
        else if (!(m_dnFadeSpp != 0 && samples >= m_dnFadeSpp)) m_hist->dnHistoryValid = false;
    };
    if (UsesRtxdi())
    {
        if (!main) return cl;
        RecordRtxdi(cl);
        denoise();
        RecordFrontReflection(cl, imageSpp);
        RecordNprLighting(cl);
        return cl;
    }
    m_hist->lastCapped = capped;
    if (main && (m_flags & 8u) && m_primed[slot])
    {
        const auto* stats = m_mapped[slot];
        LOG_INFO("[PTRAYS] eligible=%u primary=%u secondary=%u visibility=%u nonfinite=%u",
                 stats[0],stats[1],stats[2],stats[3],stats[4]);
        LOG_INFO("[PTMATERIALS] primaryHits=%u matSlotResolved=%u", stats[5], stats[6]);
        if (stats[4]) LOG_ERROR("PathTracePass: nonfinite radiance pixels=%u",stats[4]);
        if(m_areaV2)
        {
            // The [AREA] line's contract, as the pre-v2 kernels (history candidates, history won, total
            // M), then the v2 shift accounting.
            const auto* area=stats+7+2*7*128+16*32;
            const auto* v2=area+64;
            LOG_INFO("[AREA] replayed=%u historySelected=%u effectiveMTotal=%u",v2[0],v2[7],area[2]);
            LOG_INFO("[AREA_V2] history=%u replay=%u replayReverse=%u reconnect=%u reconnectMapped=%u reconnectReverse=%u reconnectReverseMapped=%u",
                     v2[0],v2[1],v2[2],v2[3],v2[4],v2[5],v2[6]);
            if(m_flags&32768u)
                LOG_INFO("[AREA_V2_SPATIAL] neighbors=%u replay=%u reconnectMapped=%u",v2[9],v2[11],v2[10]);
        }
        else if(m_flags&16384u)
        {
            const auto* area=stats+7+2*7*128+16*32;
            LOG_INFO("[AREA] replayed=%u historySelected=%u effectiveMTotal=%u",area[0],area[1],area[2]);
            if(m_flags&32768u)
                LOG_INFO("[AREA_SPATIAL] replayed=%u reverse=%u neighborSelected=%u",area[3],area[4],area[5]);
            if(m_flags&65536u)
                LOG_INFO("[AREA_FILM] shifted=%u neighbor=%u fallback=%u",area[6],area[7],area[8]);
            if(m_flags&4194304u)
                LOG_INFO("[AREA_CONTEXT] audited=%u mismatched=%u selectedHits=%u",area[12],area[13],area[14]);
            if(m_flags&134217728u)
                LOG_INFO("[AREA_RECONNECT] attempted=%u mapped=%u visible=%u contributing=%u",area[24],area[25],area[26],area[27]);
            if(m_flags&262144u)
                LOG_INFO("[AREA_OVERLAP] candidates=%u rejected=%u covered=%u",area[9],area[10],area[11]);
        }
        if((m_flags&16777216u) && !m_lensConeReported && m_hist->spp>=16)
        {
            for(uint32_t i=0;i<9;++i)
            {
                float spread; std::memcpy(&spread,stats+7+2*7*128+16*32+15+i,sizeof(spread));
                LOG_INFO("[PTLENS_CONE] sample=%u spread=%.9g",i,spread);
            }
            m_lensConeReported=true;
        }
        if((m_flags&268435456u) && !m_reconnectProbeReported && m_hist->spp>=16)
        {
            for(uint32_t i=0;i<8;++i)
            {
                const auto* v=stats+7+2*7*128+16*32+28+i*4;
                float x,y,j; std::memcpy(&x,v+1,4); std::memcpy(&y,v+2,4); std::memcpy(&j,v+3,4);
                LOG_INFO("[PTRECONNECT_PROBE] sample=%u valid=%u x=%.9g y=%.9g jacobian=%.9g",i,v[0],x,y,j);
            }
            m_reconnectProbeReported=true;
        }
        if ((m_flags & 128u) && !m_hitProbeReported && m_hist->spp>=16)
        {
            for (uint32_t i=0;i<16;++i)
            {
                std::string words;
                for(uint32_t j=0;j<32;++j)
                { if(j) words+=','; words+=std::to_string(stats[7+2*7*128+i*32+j]); }
                LOG_INFO("[RTHIT] probe=%u words=%s",i,words.c_str());
            }
            m_hitProbeReported=true;
        }
        if ((m_flags & 16u) && m_probeDispatched && !m_probeReported)
        {
            for (uint32_t c=0;c<7;++c)
                for (uint32_t bin=0;bin<128;++bin)
                {
                    float pdf; std::memcpy(&pdf,stats+7+7*128+c*128+bin,sizeof(pdf));
                    LOG_INFO("[BSDFBIN] case=%u bin=%u count=%u pdf=%.9g",c,bin,stats[7+c*128+bin],pdf);
                }
            // 32 values a line: a log message holds 1023 characters.
            static const char* const kKinds[3] = { "count", "pdf", "eval" };
            for (uint32_t c = 0; c < kDielectricProbeCases; ++c)
            {
                const uint32_t* words = stats + kDielectricProbeBase + c * kDielectricProbeCaseWords;
                for (uint32_t kind = 0; kind < 3; ++kind)
                    for (uint32_t first = 0; first < kDielectricProbeBins; first += 32)
                    {
                        std::string values;
                        char number[32];
                        for (uint32_t i = first; i < first + 32; ++i)
                        {
                            const uint32_t word = words[kind * kDielectricProbeBins + i];
                            float asFloat;
                            std::memcpy(&asFloat, &word, sizeof(asFloat));
                            if (kind == 0) std::snprintf(number, sizeof(number), "%u", word);
                            else           std::snprintf(number, sizeof(number), "%.9g", asFloat);
                            if (i != first) values += ',';
                            values += number;
                        }
                        LOG_INFO("[BSDFDIEL] case=%u kind=%s first=%u values=%s", c, kKinds[kind], first, values.c_str());
                    }
                const uint32_t* sums = words + 3 * kDielectricProbeBins;
                float r, r2, t, t2;
                std::memcpy(&r, sums + 0, 4); std::memcpy(&r2, sums + 1, 4);
                std::memcpy(&t, sums + 2, 4); std::memcpy(&t2, sums + 3, 4);
                LOG_INFO("[BSDFDIEL_SUM] case=%u r=%.9g r2=%.9g t=%.9g t2=%.9g fails=%u", c, r, r2, t, t2, sums[4]);
            }
            for (uint32_t c = 0; c < kMediaProbeHgCases; ++c)
            {
                const uint32_t* words = stats + kMediaProbeBase + c * 2 * kMediaProbeHgBins;
                LogProbeWords("MEDIAHG", c, "count", words, kMediaProbeHgBins, true);
                LogProbeWords("MEDIAHG", c, "pdf", words + kMediaProbeHgBins, kMediaProbeHgBins, false);
            }
            for (uint32_t c = 0; c < kMediaProbeFfCases; ++c)
            {
                const uint32_t* words = stats + kMediaProbeFfBase + c * kMediaProbeFfWords;
                LogProbeWords("MEDIAFF", c, "count", words, kMediaProbeFfBins, true);
                // none, over and the four source counts; then the depths, the absorption
                // transmittance and the ratio-tracking sums.
                LogProbeWords("MEDIAFF", c, "side", words + kMediaProbeFfBins, 6, true);
                LogProbeWords("MEDIAFF", c, "float", words + kMediaProbeFfBins + 6, 7, false);
            }
            for (uint32_t c = 0; c < kMediaProbeEaCases; ++c)
            {
                const uint32_t* words = stats + kMediaProbeEaBase + c * 2 * kMediaProbeEaBins;
                LogProbeWords("MEDIAEA", c, "count", words, kMediaProbeEaBins, true);
                LogProbeWords("MEDIAEA", c, "pdf", words + kMediaProbeEaBins, kMediaProbeEaBins, false);
            }
            m_probeReported = true;
        }
    }
    Constants cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_ws->fd.viewProj))));
    std::memcpy(cb.camera, &m_ws->fd.camera, 12);
    std::memcpy(cb.sunDirection, &m_ws->fd.sunDirection, 12);
    std::memcpy(cb.sunColor, &m_ws->fd.sunColor, 12);
    cb.width = m_ws->w; cb.height = m_ws->h; cb.spp = m_hist->spp;
    cb.flags = m_flags | (rrEnabled?8192u:0u); cb.constantEnvironment = m_constantEnvironment;
    if((m_flags&(65536u|4194304u)) && m_hist->areaValid && std::memcmp(&m_hist->areaViewProj,&m_ws->fd.viewProj,sizeof(m_hist->areaViewProj))!=0)
        cb.flags |= 131072u;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.areaPreviousViewProj),XMMatrixTranspose(XMLoadFloat4x4(&m_hist->areaViewProj)));
    // Camera-primary is perspective-only: the renderer gates this arm out for orthographic views
    // rather than silently using a pinhole there.
    XMFLOAT4X4 cameraWorld{};
    XMStoreFloat4x4(&cameraWorld,XMMatrixInverse(nullptr,XMLoadFloat4x4(&m_ws->fd.view)));
    std::memcpy(cb.cameraRight,&cameraWorld._11,12);
    std::memcpy(cb.cameraUp,&cameraWorld._21,12);
    std::memcpy(cb.cameraForward,&cameraWorld._31,12);
    cb.lensRadius=m_lensRadius; cb.focusDistance=m_focusDistance;
    // Reconnection has no continuous lens domain at zero aperture. Let the
    // temporal kernel publish directly: no retrace or round-trip MIS division.
    // Zero allocation is an exact temporal-only control, even with a lens.
    cb.areaReconnectWeight=m_areaReconnectWeight;
    if(m_areaV2)
    {
        if(!m_hist->prevCamValid)
        {
            XMStoreFloat4x4(&m_hist->prevInvViewProj,XMMatrixInverse(nullptr,XMLoadFloat4x4(&m_ws->fd.viewProj)));
            std::memcpy(m_hist->prevCamera,&m_ws->fd.camera,12); std::memcpy(m_hist->prevRight,&cameraWorld._11,12);
            std::memcpy(m_hist->prevUp,&cameraWorld._21,12); std::memcpy(m_hist->prevForward,&cameraWorld._31,12);
            m_hist->prevLens=m_lensRadius; m_hist->prevFocus=m_focusDistance; m_hist->prevCamValid=true;
        }
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.areaPrevInvViewProj),XMMatrixTranspose(XMLoadFloat4x4(&m_hist->prevInvViewProj)));
        std::memcpy(cb.areaPrevCamera,m_hist->prevCamera,12); std::memcpy(cb.areaPrevRight,m_hist->prevRight,12);
        std::memcpy(cb.areaPrevUp,m_hist->prevUp,12); std::memcpy(cb.areaPrevForward,m_hist->prevForward,12);
        cb.areaPrevLensRadius=m_hist->prevLens; cb.areaPrevFocus=m_hist->prevFocus;
        const bool cameraStatic=m_hist->areaValid && std::memcmp(&m_hist->areaViewProj,&m_ws->fd.viewProj,sizeof(m_hist->areaViewProj))==0
            && m_hist->prevLens==m_lensRadius && m_hist->prevFocus==m_focusDistance;
        cb.areaFlags=1u|((m_areaV2Reconnect && m_lensRadius>0.f)?2u:0u)|(cameraStatic?4u:0u)
            |((m_flags&32768u)?8u:0u)|16u
            |(m_areaNoIdentity?64u:0u) // diagnostic: every shift traced even at rest
            |((m_spatialGate && (m_flags&32768u))?32u:0u); // spatial reuse only for short histories
        cb.areaSpatialK=m_areaSpatialK; cb.areaSpatialRadius=m_areaSpatialRadius;
        cb.areaSpatialConfidence=m_areaSpatialConfidence;
    }
    if((m_flags&134217728u) && (m_lensRadius<=0.f || m_areaReconnectWeight==0.f)) cb.flags &= ~32768u;
    if((m_flags&1073741824u) && (cb.flags&32768u))
    {
        if(m_ws->areaRadianceState!=RHI::ResourceState::UNORDERED_ACCESS)
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_ws->areaRadiance,m_ws->areaRadianceState,RHI::ResourceState::UNORDERED_ACCESS),cl);
        m_ws->areaRadianceState=RHI::ResourceState::UNORDERED_ACCESS;
        cb.areaRadianceIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_ws->areaRadiance));
    }
    cb.materialsIdx=m_ws->fd.materialsIdx;
    cb.pathMaterialsIdx=m_ws->fd.pathMaterialsIdx;
    cb.sunCosRadius = std::cos(m_sunAngularRadius);
    cb.lightsIdx = m_ws->fd.lightsIdx; cb.lightCount = m_ws->fd.lightCount;
    cb.activeLocalLightCount = m_ws->fd.activeLocalLightCount;
    cb.firstActiveLocalLightIndex = m_ws->fd.firstActiveLocalLightIndex;
    // Power-proportional next-event lights (the alias table's own row count: lights with zero
    // power never enter it).
    if (m_ws->fd.lightAliasIdx && m_ws->fd.lightAliasCount && !m_uniformLights)
    {
        cb.lightAliasIdx = m_ws->fd.lightAliasIdx;
        cb.activeLocalLightCount = m_ws->fd.lightAliasCount;
    }
    if(m_flags&16384u)
    {
        for(uint32_t i=0;i<2;++i)
        {
            const auto state=i==m_hist->areaWrite?RHI::ResourceState::UNORDERED_ACCESS:RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
            if(m_hist->areaStates[i]!=state) gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaHistory[i],m_hist->areaStates[i],state),cl);
            if((m_flags&65536u) && m_hist->areaStates[i]!=state) gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaFilm[i],m_hist->areaStates[i],state),cl);
            if((m_flags&4194304u) && m_hist->areaStates[i]!=state)
            {
                gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaPrimary[i],m_hist->areaStates[i],state),cl);
                gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaGeometry[i],m_hist->areaStates[i],state),cl);
            }
            if(m_areaV2 && m_hist->areaStates[i]!=state) gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaLens[i],m_hist->areaStates[i],state),cl);
            m_hist->areaStates[i]=state;
        }
        cb.areaPreviousIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaHistory[1-m_hist->areaWrite]));
        cb.areaCurrentIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->areaHistory[m_hist->areaWrite]));
        if(m_flags&4194304u)
        {
            cb.areaPreviousPrimaryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaPrimary[1-m_hist->areaWrite]));
            cb.areaCurrentPrimaryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->areaPrimary[m_hist->areaWrite]));
            cb.areaPreviousGeometryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaGeometry[1-m_hist->areaWrite]));
            cb.areaCurrentGeometryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->areaGeometry[m_hist->areaWrite]));
        }
        cb.areaHistoryCap=m_areaHistoryCap;
        cb.areaHistoryValid=m_hist->areaValid?1u:0u;
        if(m_areaV2)
        {
            cb.areaPreviousLensIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaLens[1-m_hist->areaWrite]));
            cb.areaCurrentLensIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->areaLens[m_hist->areaWrite]));
        }
        if(m_flags&65536u)
        {
            cb.areaPreviousFilmIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaFilm[1-m_hist->areaWrite]));
            cb.areaCurrentFilmIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->areaFilm[m_hist->areaWrite]));
        }
    }
    m_effectiveBounces=capped?4u:m_maxBounces;
    m_rrStart=capped?2u:3u;
    cb.maxBounces = m_effectiveBounces; cb.ptRRStart = m_rrStart; cb.environmentScale = m_ws->fd.environmentScale;
    cb.ptLastNeeOff = m_lastNeeOff ? 1u : 0u;
    cb.ptGiShellModel = m_giShellModel ? 1u : 0u;
    if(capped && !m_bounceCapLogged)
    { LOG_INFO("[PT_BOUNCE_CAP] capped=1 maxBounces=4 rrStart=2 reason=%s",(m_flags&16384u)?"area":"unstable"); m_bounceCapLogged=true; }
    else if(!capped && m_bounceCapLogged)
    { LOG_INFO("[PT_BOUNCE_CAP] capped=0 maxBounces=%u rrStart=3",m_maxBounces); m_bounceCapLogged=false; }
    cb.skyIdx = m_ws->fd.skyIdx;
    // A free-running counter: seeding from spp / areaFrame (both reset on every camera move)
    // would replay one random stream on every moving frame, freezing the 1-spp noise pattern to
    // the screen.
    cb.seed = (m_hist->rngFrame++) ^ (m_seed*0x9e3779b9u); if(m_areaMotion) ++m_hist->areaFrame; if(rrEnabled) ++m_rrFrame;
    cb.alphaTestEnabled = m_ws->fd.alphaVA != 0; cb.alphaInstanceCount = m_ws->fd.instanceCount;
    auto index = [&](RG::RGTextureHandle handle) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(handle))); };
    cb.albedoIdx = index(m_albedo); cb.normalIdx = index(m_normal); cb.surfaceIdx = index(m_surface); cb.depthIdx = index(m_depth);
    if(rrEnabled)
    {
        uint32_t indices[4];
        for(uint32_t i=0;i<4;++i)
        {
            if(m_rrGuideStates[i]!=RHI::ResourceState::UNORDERED_ACCESS)
                gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrGuides[i],m_rrGuideStates[i],RHI::ResourceState::UNORDERED_ACCESS),cl);
            m_rrGuideStates[i]=RHI::ResourceState::UNORDERED_ACCESS;
            indices[i]=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_rrGuides[i]));
        }
        cb.rrAlbedoUav=indices[0]; cb.rrSpecularUav=indices[1]; cb.rrNormalUav=indices[2]; cb.rrDistanceUav=indices[3];
    }
    // The fog as a participating medium: the media variants of the entries (PT_MEDIA).
    const bool media = m_ws->fd.mediumIdx != 0 && BuildMediaPipelines();
    if (media) WriteMediaConstants(cb);
    {
        const float sun[4] = { m_ws->fd.sunColor.x, m_ws->fd.sunColor.y, m_ws->fd.sunColor.z, m_ws->fd.sunMediumScale };
        const uint64_t key = (media ? 1ull : 0ull) | (m_ws->fogWanted ? 2ull : 0ull) | (uint64_t(cb.mediaFlags) << 2) |
            (m_ws->fd.lightMediumScaleIdx ? 16ull : 0ull) | (Sys::WordHash64(sun, sizeof(sun)) << 5);
        if (main && key != m_mediaLogKey)
        {
            m_mediaLogKey = key;
            LOG_INFO("[PT_MEDIA] traced=%u cameraFog=%u single=%u equiAngular=%u lightScales=%u sunScale=%.4f sun=%.6f_%.6f_%.6f",
                     media ? 1u : 0u, m_ws->fogWanted ? 1u : 0u, (cb.mediaFlags & 1u) ? 1u : 0u, (cb.mediaFlags & 2u) ? 1u : 0u,
                     m_ws->fd.lightMediumScaleIdx ? 1u : 0u, m_ws->fd.sunMediumScale, m_ws->fd.sunColor.x, m_ws->fd.sunColor.y, m_ws->fd.sunColor.z);
        }
    }
    // The preview denoiser's sample moment, on the image's accumulation rule (RTStoreSample). It
    // means something from the frame that restarts the image with it on: before that (just
    // allocated, or skipped while Area traced) the denoiser estimates the variance spatially.
    if (m_dnWanted && m_hist->dnSampleMoment.IsValid() && !(m_flags & 16384u))
    {
        cb.auxOutUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->dnSampleMoment));
        if (imageSpp == 0) m_hist->dnMomentValid = true;
    }
    else m_hist->dnMomentValid = false;
    *m_cb.Current(gfx) = cb;
    // The accumulation UAV is shared between frame slots on the graphics queue.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hist->accum), cl);
    // The pass's layout, through the image's pipeline: every entry below is
    // built on it, so the arguments hold across their binds.
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_cb.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(kRootTLAS, m_ws->fd.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(kRootInstances, m_ws->fd.instanceVA, cl);
    gfx.SetComputeRootSRVAddress(kRootAlpha, m_ws->fd.alphaVA, cl);
    gfx.SetComputeDescriptorTable(kRootBuffers, m_ws->fd.buffers, cl);
    gfx.SetComputeDescriptorTable(kRootTextures, gfx.GetBindlessTextureTableGpuHandle(), cl);
    gfx.SetComputeDescriptorTable(kRootResult, gfx.GetTextureUAVGpuHandle(*m_graph.GetPhysicalTexture(m_result)), cl);
    gfx.SetComputeDescriptorTable(kRootAccum, gfx.GetTextureUAVGpuHandle(m_hist->accum), cl);
    if (m_statsState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_stats,m_statsState,RHI::ResourceState::UNORDERED_ACCESS),cl);
    m_statsState = RHI::ResourceState::UNORDERED_ACCESS;
    gfx.SetComputeDescriptorTable(kRootStats, gfx.GetBufferUAVGpuHandle(m_stats),cl);
    if (main && (m_flags & 128u) && !m_hitProbeReported && m_hist->spp>=8)
    {
        gfx.BindComputePipelineState(m_hitProbe, cl); gfx.DispatchCompute(1,1,1,cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
    }
    if (main && (m_flags & 16u) && !m_probeDispatched)
    {
        gfx.BindComputePipelineState(m_clearProbe, cl); gfx.DispatchCompute(1,7,1,cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_probe, cl); gfx.DispatchCompute(4096,7,1,cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_dielectricProbeClear, cl);
        gfx.DispatchCompute(1, kDielectricProbeCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_dielectricProbe, cl);
        gfx.DispatchCompute(kDielectricProbeGroups, kDielectricProbeCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_dielectricProbeReduce, cl);
        gfx.DispatchCompute(1, kDielectricProbeCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_mediaProbeClear, cl);
        gfx.DispatchCompute(1, kMediaProbeHgCases + kMediaProbeFfCases + kMediaProbeEaCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_mediaProbeHg, cl);
        gfx.DispatchCompute(kMediaProbeGroups, kMediaProbeHgCases, 1, cl);
        gfx.BindComputePipelineState(m_mediaProbeFreeFlight, cl);
        gfx.DispatchCompute(kMediaProbeGroups, kMediaProbeFfCases, 1, cl);
        gfx.BindComputePipelineState(m_mediaProbeEquiAngular, cl);
        gfx.DispatchCompute(kMediaProbeGroups, kMediaProbeEaCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        gfx.BindComputePipelineState(m_mediaProbeReduce, cl);
        gfx.DispatchCompute(1, kMediaProbeFfCases, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        m_probeDispatched=true;
    }
    if (m_flags & 8u)
    {
        gfx.BindComputePipelineState(m_clearStats, cl); gfx.DispatchCompute(1,1,1,cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
    }
    gfx.BindComputePipelineState(media ? m_mediaPso : m_pso, cl);
    gfx.DispatchCompute((m_ws->w + 7) / 8, (m_ws->h + 7) / 8, 1, cl);
    if(cb.flags&32768u)
    {
        // Temporal output becomes immutable input; spatial output never feeds history.
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaHistory[m_hist->areaWrite],m_hist->areaStates[m_hist->areaWrite],
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
        m_hist->areaStates[m_hist->areaWrite]=RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        cb.areaPreviousIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaHistory[m_hist->areaWrite]));
        if(m_flags&65536u)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaFilm[m_hist->areaWrite],RHI::ResourceState::UNORDERED_ACCESS,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            cb.areaPreviousFilmIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaFilm[m_hist->areaWrite]));
        }
        if(m_flags&4194304u)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaPrimary[m_hist->areaWrite],RHI::ResourceState::UNORDERED_ACCESS,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaGeometry[m_hist->areaWrite],RHI::ResourceState::UNORDERED_ACCESS,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            cb.areaPreviousPrimaryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaPrimary[m_hist->areaWrite]));
            cb.areaPreviousGeometryIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaGeometry[m_hist->areaWrite]));
        }
        if(m_areaV2)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->areaLens[m_hist->areaWrite],RHI::ResourceState::UNORDERED_ACCESS,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            cb.areaPreviousLensIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_hist->areaLens[m_hist->areaWrite]));
            // The spatial pass blends into the accumulation the temporal pass wrote.
            gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hist->accum), cl);
        }
        if(m_flags&1073741824u)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_ws->areaRadiance,m_ws->areaRadianceState,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            m_ws->areaRadianceState=RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
            cb.areaRadianceIdx=gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_ws->areaRadiance));
        }
        *m_areaSpatialCb.Current(gfx)=cb;
        // A separate CB is required: rewriting m_cb would change BOTH dispatches.
        gfx.SetComputeRootCBV(kRootCB, m_areaSpatialCb.CurrentBuffer(gfx),0,cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_stats), cl);
        const auto timer=gfx.BeginGPUTimestamp(cl,"PT.AreaSpatial");
        gfx.BindComputePipelineState(media ? m_mediaSpatialPso : m_areaSpatialPso, cl);
        gfx.DispatchCompute((m_ws->w+7)/8,(m_ws->h+7)/8,1,cl);
        gfx.EndGPUTimestamp(cl,timer);
    }
    if (media && m_ws->fogWanted) RecordCameraFog(cl, imageSpp);
    denoise();
    // The fog's filter after the image's, whose a-trous ping-pong it borrows. A frame skipped
    // because the fog has faded into its running mean keeps the history, as the image's does.
    if (media && m_ws->fogWanted && m_ws->fogDenoise) RecordFogDenoise(cl);
    else if (!(m_dnFadeSpp != 0 && m_ws->fogSamples >= m_dnFadeSpp)) m_hist->fogDnHistoryValid = false;
    if (main)
    {
        RecordFrontReflection(cl, imageSpp);
        RecordNprLighting(cl);
    }
    if(m_flags&16384u) m_hist->areaWrite=1-m_hist->areaWrite;
    if(rrEnabled)
        for(uint32_t i=0;i<4;++i)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrGuides[i],m_rrGuideStates[i],RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            m_rrGuideStates[i]=RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }
    if (m_ws->reference && !rrEnabled)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->accum, RHI::ResourceState::UNORDERED_ACCESS,
                        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        NrdDenoiser::Params np{};
        std::memcpy(np.worldToView,&m_ws->fd.view,sizeof(np.worldToView));
        std::memcpy(np.viewToClip,&m_ws->fd.projection,sizeof(np.viewToClip));
        np.width=m_ws->w; np.height=m_ws->h; np.frameIndex=m_hist->spp; np.resetHistory=m_hist->spp==0;
        NrdDenoiser::Binding bindings[2]{};
        bindings[0].type=nrd::ResourceType::IN_SIGNAL;
        bindings[0].tex={&m_hist->accum,RHI::ResourceState::SHADER_RESOURCE_COMPUTE};
        bindings[1].type=nrd::ResourceType::OUT_SIGNAL;
        bindings[1].tex={m_graph.GetPhysicalTexture(m_result),RHI::ResourceState::UNORDERED_ACCESS};
        const auto timer=gfx.BeginGPUTimestamp(cl,"PT.Reference");
        if (!m_ws->reference->Denoise(gfx,cl,np,bindings,2)) LOG_ERROR("PathTracePass: NRD REFERENCE dispatch failed");
        gfx.EndGPUTimestamp(cl,timer);
        gfx.BindDescriptorHeaps(cl);
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_hist->accum, RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
    }
    if (main && (m_flags & 8u))
    {
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_stats,m_statsState,RHI::ResourceState::COPY_SRC),cl);
        m_statsState = RHI::ResourceState::COPY_SRC;
        gfx.CopyBuffer(m_stats,m_readback[slot],m_statsBytes,cl);
        m_primed[slot] = true;
    }
    if (m_ws->fd.stable) ++m_hist->spp;
    m_hist->areaValid=(m_flags&16384u) && (m_ws->fd.stable || m_areaV2);
    m_hist->areaViewProj=m_ws->fd.viewProj;
    if(m_areaV2)
    {
        XMStoreFloat4x4(&m_hist->prevInvViewProj,XMMatrixInverse(nullptr,XMLoadFloat4x4(&m_ws->fd.viewProj)));
        std::memcpy(m_hist->prevCamera,&m_ws->fd.camera,12); std::memcpy(m_hist->prevRight,cb.cameraRight,12);
        std::memcpy(m_hist->prevUp,cb.cameraUp,12); std::memcpy(m_hist->prevForward,cb.cameraForward,12);
        m_hist->prevLens=m_lensRadius; m_hist->prevFocus=m_focusDistance; m_hist->prevCamValid=true;
    }
    m_hist->resetPending = false;
    if (!main) return cl;   // the published counts and the [PT] line are the main view's
    m_publishedSamples.store(m_hist->spp,std::memory_order_relaxed);
    m_publishedResets.store(m_hist->resets,std::memory_order_relaxed);
    if (m_hist->spp <= 1 || m_hist->spp % 30 == 0)
        LOG_INFO("[PT] executed=1 spp=%u resets=%u stable=%u width=%u height=%u maxBounces=%u rrOnly=%u effectiveBounces=%u rrStart=%u powerLights=%u",
            m_hist->spp, m_hist->resets, m_ws->fd.stable ? 1u : 0u, m_ws->w, m_ws->h, m_maxBounces, m_maxBounces == 0 ? 1u : 0u, m_effectiveBounces, m_rrStart,
            (m_ws->fd.lightAliasIdx && m_ws->fd.lightAliasCount && !m_uniformLights) ? m_ws->fd.lightAliasCount : 0u);
    return cl;
}

// The rtxdi mode: PTRtxdiCore fills PathTraceResult and the progressive accumulation from
// RTXDI's ReSTIR PT over the engine path tracer; the pass keeps its bookkeeping (spp, resets,
// the [PT] line).
void PathTracePass::RecordRtxdi(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    const auto slot = gfx.GetFrameIndex();
    if (m_flags & 8u) m_rtxdi->LogCounters(slot);
    if (!m_rtxdiLogged)
    {
        LOG_INFO("[PT_RTXDI_SETTINGS] %s render=%ux%u", m_rtxdi->Describe(), m_ws->w, m_ws->h);
        m_rtxdiLogged = true;
    }
    auto srv = [&](RG::RGTextureHandle h) -> uint32_t {
        if (!h.IsValid()) return 0u;
        const auto* t = m_graph.GetPhysicalTexture(h);
        return t && t->IsValid() ? gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*t)) : 0u;
    };
    PTRtxdi::Frame f{};
    f.gfx = &gfx; f.cl = cl;
    f.tlasVA = m_ws->fd.tlasVA; f.instanceVA = m_ws->fd.instanceVA; f.alphaVA = m_ws->fd.alphaVA; f.buffers = m_ws->fd.buffers;
    f.width = m_ws->w; f.height = m_ws->h;
    f.viewProj = m_ws->fd.viewProj; f.view = m_ws->fd.view; f.camera = m_ws->fd.camera;
    f.depthIdx = srv(m_depth); f.normalIdx = srv(m_normal); f.albedoIdx = srv(m_albedo); f.surfaceIdx = srv(m_surface);
    f.velocityIdx = srv(m_velocity);
    f.materialsIdx = m_ws->fd.materialsIdx; f.pathMaterialsIdx = m_ws->fd.pathMaterialsIdx;
    f.lightsIdx = m_ws->fd.lightsIdx; f.lightCount = m_ws->fd.lightCount;
    f.prevLightsIdx = m_ws->fd.prevLightsIdx; f.prevLightCount = m_ws->fd.prevLightCount; f.lightMapIdx = m_ws->fd.lightMapIdx;
    if (m_ws->fd.lightAliasIdx && m_ws->fd.lightAliasCount && !m_uniformLights)
    { f.lightAliasIdx = m_ws->fd.lightAliasIdx; f.activeLocalLightCount = m_ws->fd.lightAliasCount; }
    f.sunDirection = m_ws->fd.sunDirection; f.sunColor = m_ws->fd.sunColor; f.sunCosRadius = std::cos(m_sunAngularRadius);
    f.skyIdx = m_ws->fd.skyIdx; f.environmentScale = m_ws->fd.environmentScale; f.constantEnvironment = m_constantEnvironment;
    f.engineFlags = m_flags & (1u | 2u | 4u | 8u | 64u);
    f.glass = m_ws->fd.transmissive;
    f.alphaTestEnabled = m_ws->fd.alphaVA != 0; f.alphaInstanceCount = m_ws->fd.instanceCount;
    const auto* out = m_graph.GetPhysicalTexture(m_result);
    f.resultUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(*out));
    f.resultTable = gfx.GetTextureUAVGpuHandle(*out);
    f.accumUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_hist->accum));
    f.diResultIdx = m_ws->fd.diActive ? srv(m_diResult) : 0u;
    f.mediumIdx = m_ws->fd.rtxdiMediumIdx;
    f.mediaFlags = RtxdiMediaRule();
    f.froxel = m_ws->fd.rtxdiFroxel;
    f.seed = m_seed;
    f.spp = m_hist->spp;
    // RR denoises one sample per frame: no progressive accumulation under it.
    f.accumulate = m_ws->fd.stable && !(m_flags & (32u | 4096u)) && !m_rrEnabled;
    f.reset = false;   // ResetHistory() resets the core's history itself
    if (m_rrEnabled)
    {
        // The core's Shade writes PathTrace.cs.hlsl's four guides.
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (m_rrGuideStates[i] != RHI::ResourceState::UNORDERED_ACCESS)
                gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrGuides[i], m_rrGuideStates[i], RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_rrGuideStates[i] = RHI::ResourceState::UNORDERED_ACCESS;
            f.rrGuideUav[i] = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(m_rrGuides[i]));
        }
    }
    // The accumulation UAV is shared between frame slots.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_hist->accum), cl);
    m_rtxdi->Record(f);
    if (m_rrEnabled)
        for (uint32_t i = 0; i < 4; ++i)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrGuides[i], m_rrGuideStates[i], RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
            m_rrGuideStates[i] = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        }
    if (m_ws->fd.stable) ++m_hist->spp;
    m_hist->resetPending = false;
    m_publishedSamples.store(m_hist->spp, std::memory_order_relaxed);
    m_publishedResets.store(m_hist->resets, std::memory_order_relaxed);
    if (m_hist->spp <= 1 || m_hist->spp % 30 == 0)
        LOG_INFO("[PT] executed=1 spp=%u resets=%u stable=%u width=%u height=%u maxBounces=%u rrOnly=0 effectiveBounces=%u rrStart=%u powerLights=%u backend=rtxdi di=%u",
            m_hist->spp, m_hist->resets, m_ws->fd.stable ? 1u : 0u, m_ws->w, m_ws->h, m_maxBounces, m_maxBounces, m_rrStart,
            f.activeLocalLightCount, f.diResultIdx ? 1u : 0u);
}

// The constants an auxiliary entry (FrontReflection, NprAmbient) shares with either core's
// image: the camera, sun, lights, environment and materials, the diffuse /
// constant-environment / no-sun-NEE / direct-only arms, and the image's path length with the
// entry's surface as vertex 0 (the bounce cap while the scene does not hold still). Each
// entry adds its own flags, seed and targets.
PathTracePass::Constants PathTracePass::AuxConstants(uint32_t width, uint32_t height) const
{
    Constants cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_ws->fd.viewProj))));
    std::memcpy(cb.camera, &m_ws->fd.camera, 12);
    std::memcpy(cb.sunDirection, &m_ws->fd.sunDirection, 12);
    std::memcpy(cb.sunColor, &m_ws->fd.sunColor, 12);
    cb.width = width; cb.height = height;
    cb.flags = m_flags & (1u | 2u | 4u | 64u);
    cb.constantEnvironment = m_constantEnvironment;
    cb.sunCosRadius = std::cos(m_sunAngularRadius);
    cb.skyIdx = m_ws->fd.skyIdx; cb.environmentScale = m_ws->fd.environmentScale;
    cb.lightsIdx = m_ws->fd.lightsIdx; cb.lightCount = m_ws->fd.lightCount;
    cb.activeLocalLightCount = m_ws->fd.activeLocalLightCount;
    cb.firstActiveLocalLightIndex = m_ws->fd.firstActiveLocalLightIndex;
    if (m_ws->fd.lightAliasIdx && m_ws->fd.lightAliasCount && !m_uniformLights)
    { cb.lightAliasIdx = m_ws->fd.lightAliasIdx; cb.activeLocalLightCount = m_ws->fd.lightAliasCount; }
    cb.materialsIdx = m_ws->fd.materialsIdx; cb.pathMaterialsIdx = m_ws->fd.pathMaterialsIdx;
    cb.alphaTestEnabled = m_ws->fd.alphaVA != 0; cb.alphaInstanceCount = m_ws->fd.instanceCount;
    const bool capped = !m_ws->fd.stable && m_referenceFromSettings && !m_bounceCapCli;
    cb.maxBounces = capped ? 4u : m_maxBounces; cb.ptRRStart = capped ? 2u : 3u;
    cb.ptLastNeeOff = m_lastNeeOff ? 1u : 0u;
    return cb;
}

// Every root parameter for an auxiliary entry: after the rtxdi core the list
// holds that core's root signature, and the entries declare the image's
// targets and counters too.
void PathTracePass::BindAuxRoot(IGraphicsDevice& gfx, RHI::CommandList cl, const RHI::GPUBuffer& constants,
                                const RHI::PipelineState& pso)
{
    gfx.BindComputePipelineState(pso, cl);   // the pass's layout (every entry is built on it)
    gfx.SetComputeRootCBV(kRootCB, constants, 0, cl);
    gfx.SetComputeRootSRVAddress(kRootTLAS, m_ws->fd.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(kRootInstances, m_ws->fd.instanceVA, cl);
    gfx.SetComputeRootSRVAddress(kRootAlpha, m_ws->fd.alphaVA, cl);
    gfx.SetComputeDescriptorTable(kRootBuffers, m_ws->fd.buffers, cl);
    gfx.SetComputeDescriptorTable(kRootTextures, gfx.GetBindlessTextureTableGpuHandle(), cl);
    gfx.SetComputeDescriptorTable(kRootResult, gfx.GetTextureUAVGpuHandle(*m_graph.GetPhysicalTexture(m_result)), cl);
    gfx.SetComputeDescriptorTable(kRootAccum, gfx.GetTextureUAVGpuHandle(m_hist->accum), cl);
    if (m_statsState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_stats, m_statsState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_statsState = RHI::ResourceState::UNORDERED_ACCESS;
    gfx.SetComputeDescriptorTable(kRootStats, gfx.GetBufferUAVGpuHandle(m_stats), cl);
}

// The front-layer reflection (PathTrace.cs.hlsl FrontReflection), after either
// core's image: the same scene, lights and path length, its own constants and
// accumulation. imageSpp is the sample index the image used this frame.
void PathTracePass::RecordFrontReflection(RHI::CommandList cl, uint32_t imageSpp)
{
    if (!m_frontActive || !FrontReflectionReady()) { m_frontValid = false; return; }
    IGraphicsDevice& gfx = cl.GetDevice();
    // Restart with the image, and after a frame the stamp was not traced.
    if (imageSpp == 0 || !m_frontValid) m_frontSpp = 0;
    // Accumulate while the image does (a still scene, no RR): the running mean converges to the
    // reference; every other frame denoises its own sample. A denoised frame after accumulated
    // ones starts a new history.
    const bool denoise = m_rrEnabled || !m_ws->fd.stable || (m_flags & 4096u);
    if (!denoise) m_frontNrdValid = false;
    Constants cb = AuxConstants(m_frontW, m_frontH);
    cb.spp = m_frontSpp;
    // Beyond the shared arms: 4096 = no accumulation, as for the image, and 8192 = the frame's
    // sample for the denoiser (DenoiseFrontReflection), ptFrontSamples per mirror-like texel then
    // (REBLUR cannot blur a mirror spatially, so its variance is what reaches the screen).
    cb.flags |= (m_flags & 4096u) | (denoise ? 8192u : 0u);
    cb.ptFrontSamples = (denoise ? m_frontMirrorSamples : 1u) | (m_frontNeeCandidates << 8);
    cb.seed = (m_frontFrame++) ^ (m_seed * 0x9e3779b9u);
    auto srv = [&](RG::RGTextureHandle h) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(h))); };
    cb.depthIdx = srv(m_frontDepth); cb.normalIdx = srv(m_frontNormal); cb.surfaceIdx = srv(m_frontSurface);
    // The front tier's texture is written here (the running mean) or by the denoiser's resolve
    // (a frame that does not accumulate); the sample itself then goes to m_frontNoisy.
    if (m_frontState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_front, m_frontState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_frontState = RHI::ResourceState::UNORDERED_ACCESS;
    if (denoise)
    {
        if (m_frontNrdStates[0] != RHI::ResourceState::UNORDERED_ACCESS)
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_frontNoisy, m_frontNrdStates[0], RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_frontNrdStates[0] = RHI::ResourceState::UNORDERED_ACCESS;
    }
    cb.auxOutUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(denoise ? m_frontNoisy : m_front));
    *m_frontCb.Current(gfx) = cb;
    BindAuxRoot(gfx, cl, m_frontCb.CurrentBuffer(gfx), m_frontPso);
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.FrontReflection");
    gfx.DispatchCompute((m_frontW + 7) / 8, (m_frontH + 7) / 8, 1, cl);
    gfx.EndGPUTimestamp(cl, timer);
    if (denoise) DenoiseFrontReflection(cl);
    // The forward glass draws read it later in this graph (bindless, LightCB).
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_front, m_frontState, RHI::ResourceState::SHADER_RESOURCE), cl);
    m_frontState = RHI::ResourceState::SHADER_RESOURCE;
    m_frontValid = true;
    m_frontSpp = denoise ? 0 : m_frontSpp + 1;
    // Every 120th sample, or frame while the image does not accumulate.
    if (m_frontSpp == 1 || (m_frontSpp && m_frontSpp % 120 == 0) || (!m_frontSpp && m_frontFrame % 120 == 1))
        LOG_INFO("[PT_FRONT] executed=1 spp=%u imageSpp=%u stable=%u rr=%u denoised=%u mirrorSamples=%u width=%u height=%u nee=%u",
            m_frontSpp, imageSpp, m_ws->fd.stable ? 1u : 0u, m_rrEnabled ? 1u : 0u, denoise ? 1u : 0u,
            cb.ptFrontSamples & 0xffu, m_frontW, m_frontH, m_frontNeeCandidates);
}

// A frame that does not accumulate (RR, or a scene that moves): the frame's
// sample (m_frontNoisy) -> PathTraceFrontNRD.cs.hlsl Pack ->
// REBLUR_SPECULAR on the pane's own surface -> Resolve into the front texture.
// Its history restarts after a frame the stamp was not traced and on a content cut
// (FrameData::contentCut); the scene changing under it (skinned characters in the TLAS) does
// not.
void PathTracePass::DenoiseFrontReflection(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    // NRD's hitDistanceParameters.A: the library default, here and in Pack.
    constexpr float kFrontHitDistA = 3.f;
    RHI::Texture* const textures[6] = { &m_frontNoisy, &m_frontNrdNormalRough, &m_frontNrdViewZ,
                                        &m_frontNrdMV, &m_frontNrdSpecIn, &m_frontNrdSpecOut };
    auto transition = [&](uint32_t i, RHI::ResourceState state)
    {
        if (m_frontNrdStates[i] != state)
            gfx.PushBarrier(RHI::GPUBarrier::Image(textures[i], m_frontNrdStates[i], state), cl);
        m_frontNrdStates[i] = state;
    };
    auto srv = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(t)); };
    auto uav = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(t)); };
    auto graphSrv = [&](RG::RGTextureHandle h) { return srv(*m_graph.GetPhysicalTexture(h)); };
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.FrontDenoise");

    // ---- Pack: the stamp + the sample -> NRD's inputs ----
    transition(0, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    for (uint32_t i = 1; i <= 4; ++i) transition(i, RHI::ResourceState::UNORDERED_ACCESS);
    FrontNrdConstants c{};
    c.width = m_frontW; c.height = m_frontH;
    c.depthIdx = graphSrv(m_frontDepth); c.normalIdx = graphSrv(m_frontNormal);
    c.surfaceIdx = graphSrv(m_frontSurface); c.velocityIdx = graphSrv(m_frontVelocity);
    c.noisyIdx = srv(m_frontNoisy);
    c.normalRoughUav = uav(m_frontNrdNormalRough); c.viewZUav = uav(m_frontNrdViewZ);
    c.mvUav = uav(m_frontNrdMV); c.specInUav = uav(m_frontNrdSpecIn);
    c.specOutIdx = srv(m_frontNrdSpecOut); c.frontUav = uav(m_front);
    c.nearZ = m_ws->fd.nearZ; c.farZ = m_ws->fd.farZ; c.hitDistA = kFrontHitDistA;
    *m_frontNrdCb.Current(gfx) = c;
    gfx.SetComputeRootCBV(kRootCB, m_frontNrdCb.CurrentBuffer(gfx), 0, cl);
    gfx.BindComputePipelineState(m_frontPackPso, cl);
    gfx.DispatchCompute((m_frontW + 7) / 8, (m_frontH + 7) / 8, 1, cl);

    // ---- REBLUR_SPECULAR (inputs NPSR, output UAV; NRD restores them) ----
    for (uint32_t i = 1; i <= 4; ++i) transition(i, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(5, RHI::ResourceState::UNORDERED_ACCESS);
    NrdDenoiser::Params np{};
    std::memcpy(np.viewToClip, &m_ws->fd.projection, sizeof(np.viewToClip));
    std::memcpy(np.worldToView, &m_ws->fd.view, sizeof(np.worldToView));
    np.jitter[0] = m_ws->fd.jitterX; np.jitter[1] = m_ws->fd.jitterY;
    np.frameIndex = m_frontNrdFrame++;
    np.resetHistory = !m_frontNrdValid || !m_frontValid || m_ws->fd.contentCut;
    np.denoisingRange = m_ws->fd.farZ * 0.99f;
    np.width = m_frontW; np.height = m_frontH;
    np.specHitDistScaleA = kFrontHitDistA;
    auto bind = [&](nrd::ResourceType type, const RHI::Texture& t, RHI::ResourceState state)
    {
        NrdDenoiser::Binding b; b.type = type;
        b.tex.texture = &t; b.tex.state = state;
        return b;
    };
    const NrdDenoiser::Binding bindings[] = {
        bind(nrd::ResourceType::IN_MV,                     m_frontNrdMV,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS,       m_frontNrdNormalRough, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_VIEWZ,                  m_frontNrdViewZ,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,  m_frontNrdSpecIn,      RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, m_frontNrdSpecOut,     RHI::ResourceState::UNORDERED_ACCESS),
    };
    const bool denoised = m_frontNrd->Denoise(gfx, cl, np, bindings, 5);
    // NRD replaced the descriptor heaps and the root signature: the layout
    // comes back with the resolve's pipeline.
    gfx.BindDescriptorHeaps(cl);
    gfx.BindComputePipelineState(m_frontResolvePso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_frontNrdCb.CurrentBuffer(gfx), 0, cl);

    // ---- Resolve: NRD's output -> the front tier (m_front is UAV here) ----
    transition(5, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    if (denoised)
        gfx.DispatchCompute((m_frontW + 7) / 8, (m_frontH + 7) / 8, 1, cl);
    else
    {
        // The front tier must not show a stale pane: drop out next frame.
        m_frontFailed = true;
        LOG_ERROR("PathTracePass: front-layer reflection NRD dispatch failed");
    }
    m_frontNrdValid = denoised;
    gfx.EndGPUTimestamp(cl, timer);
}

// PT-lit toon receivers, after either core's image: NprAmbient's sample on the
// GBuffer (non-toon pixels write a = -1), its temporal accumulation through
// the GBuffer motion (ping-pong) and the separable spatial pass into the
// texture Lighting.ps's NPR variant reads (LightCB.nprPtAmbientIdx). The
// history restarts after a frame it was not run and on a content cut (FrameData::contentCut).
void PathTracePass::RecordNprLighting(RHI::CommandList cl)
{
    if (!m_nprActive || !NprLightingReady()) { m_hist->nprValid = false; return; }
    IGraphicsDevice& gfx = cl.GetDevice();
    RHI::Texture* const textures[7] = { &m_nprNoisy, &m_hist->nprHistory[0], &m_hist->nprHistory[1],
                                        &m_hist->nprMeta[0], &m_hist->nprMeta[1], &m_nprSpatial, &m_nprFinal };
    auto transition = [&](uint32_t i, RHI::ResourceState state)
    {
        if (NprState(i) != state)
            gfx.PushBarrier(RHI::GPUBarrier::Image(textures[i], NprState(i), state), cl);
        NprState(i) = state;
    };
    auto srv = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(t)); };
    auto uav = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(t)); };
    auto graphSrv = [&](RG::RGTextureHandle h) { return srv(*m_graph.GetPhysicalTexture(h)); };
    const uint32_t write = m_hist->nprWrite, read = 1u - m_hist->nprWrite;
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.NprLighting");

    // ---- The sample ----
    Constants cb = AuxConstants(m_nprW, m_nprH);
    cb.seed = (m_hist->nprFrame++) ^ (m_seed * 0x9e3779b9u);
    cb.depthIdx = graphSrv(m_depth); cb.normalIdx = graphSrv(m_normal);
    transition(0, RHI::ResourceState::UNORDERED_ACCESS);
    cb.auxOutUav = uav(m_nprNoisy);
    *m_nprCb.Current(gfx) = cb;
    BindAuxRoot(gfx, cl, m_nprCb.CurrentBuffer(gfx), m_nprPso);
    gfx.DispatchCompute((m_nprW + 7) / 8, (m_nprH + 7) / 8, 1, cl);

    // ---- Temporal: last frame's accumulation -> this frame's ----
    transition(0, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(1 + read, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(3 + read, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(1 + write, RHI::ResourceState::UNORDERED_ACCESS);
    transition(3 + write, RHI::ResourceState::UNORDERED_ACCESS);
    NprDenoiseConstants c{};
    c.width = m_nprW; c.height = m_nprH;
    c.depthIdx = graphSrv(m_depth); c.velocityIdx = graphSrv(m_velocity);
    c.noisyIdx = srv(m_nprNoisy);
    c.historyIdx = srv(m_hist->nprHistory[read]); c.historyMetaIdx = srv(m_hist->nprMeta[read]);
    c.historyOutUav = uav(m_hist->nprHistory[write]); c.historyMetaOutUav = uav(m_hist->nprMeta[write]);
    c.nearZ = m_ws->fd.nearZ; c.farZ = m_ws->fd.farZ;
    c.reset = (!m_hist->nprValid || m_ws->fd.contentCut) ? 1u : 0u;
    *m_nprDenoiseCb[0].Current(gfx) = c;
    gfx.SetComputeRootCBV(kRootCB, m_nprDenoiseCb[0].CurrentBuffer(gfx), 0, cl);
    gfx.BindComputePipelineState(m_nprTemporalPso, cl);
    gfx.DispatchCompute((m_nprW + 7) / 8, (m_nprH + 7) / 8, 1, cl);

    // ---- Spatial: horizontal into the intermediate, vertical into the output ----
    gfx.BindComputePipelineState(m_nprSpatialPso, cl);
    transition(1 + write, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(5, RHI::ResourceState::UNORDERED_ACCESS);
    c.spatialInIdx = srv(m_hist->nprHistory[write]); c.spatialOutUav = uav(m_nprSpatial); c.direction = 0;
    *m_nprDenoiseCb[1].Current(gfx) = c;
    gfx.SetComputeRootCBV(kRootCB, m_nprDenoiseCb[1].CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute((m_nprW + 7) / 8, (m_nprH + 7) / 8, 1, cl);
    transition(5, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    transition(6, RHI::ResourceState::UNORDERED_ACCESS);
    c.spatialInIdx = srv(m_nprSpatial); c.spatialOutUav = uav(m_nprFinal); c.direction = 1;
    *m_nprDenoiseCb[2].Current(gfx) = c;
    gfx.SetComputeRootCBV(kRootCB, m_nprDenoiseCb[2].CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute((m_nprW + 7) / 8, (m_nprH + 7) / 8, 1, cl);
    // Lighting.ps reads it later in this graph (bindless, LightCB).
    transition(6, RHI::ResourceState::SHADER_RESOURCE);
    gfx.EndGPUTimestamp(cl, timer);
    m_hist->nprWrite = read;
    m_hist->nprValid = true;
    if (m_hist->nprFrame % 120 == 1)
        LOG_INFO("[PT_NPR] executed=1 frame=%u width=%u height=%u", m_hist->nprFrame, m_nprW, m_nprH);
}

bool PathTracePass::DenoiseRuns(uint32_t samples, bool restarted) const
{
    if (!m_dnWanted || m_dnFailed || !m_hist->dnSampleMoment.IsValid() || !m_dnTemporalPso.IsValid() || !m_dnAtrousPso.IsValid())
        return false;
    // RR denoises itself; camera rays and Area have no GBuffer primary to guide the filter; the NRD
    // reference writes the image after the trace.
    if (m_rrEnabled || (m_flags & (32u | 512u | 16384u)) || !m_velocity.IsValid() || m_hist->dnW != m_ws->w || m_hist->dnH != m_ws->h)
        return false;
    return m_dnFadeSpp == 0 ? restarted : samples < m_dnFadeSpp;
}

// The preview denoiser over the image the trace just wrote: Temporal into the history and the
// a-trous ping-pong, then four a-trous passes, the last writing the image. @p samples: the image's
// sample count after this frame; @p restarted: this frame's image is one sample.
void PathTracePass::RecordDenoise(RHI::CommandList cl, uint32_t samples, bool restarted)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    auto srv = [&](RG::RGTextureHandle h)
    { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(h))); };
    auto uav = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(t)); };
    const RHI::Texture* image = m_graph.GetPhysicalTexture(m_result);
    const uint32_t write = m_hist->dnWrite, read = 1u - m_hist->dnWrite;
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.Denoise");
    // The trace wrote the image and the sample moment. Every pass below reads what the one before
    // wrote and writes what an earlier one read, all through UAVs: one global UAV barrier each.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);

    DenoiseConstants c{};
    c.width = m_ws->w; c.height = m_ws->h;
    c.depthIdx = srv(m_depth); c.normalIdx = srv(m_normal); c.albedoIdx = srv(m_albedo); c.velocityIdx = srv(m_velocity);
    c.resultUav = uav(*image);
    c.sampleMomentUav = (!UsesRtxdi() && m_hist->dnMomentValid) ? uav(m_hist->dnSampleMoment) : 0u;   // the rtxdi core keeps none
    c.historyUav = uav(m_hist->dnHistory[read]); c.historyMomentsUav = uav(m_hist->dnMoments[read]); c.historyMetaUav = uav(m_hist->dnMeta[read]);
    c.historyOutUav = uav(m_hist->dnHistory[write]); c.historyMomentsOutUav = uav(m_hist->dnMoments[write]);
    c.historyMetaOutUav = uav(m_hist->dnMeta[write]);
    c.samples = samples;
    const bool historyValid = m_hist->dnHistoryValid && !m_ws->fd.contentCut && !m_ws->fd.cameraCut;
    const uint32_t baseFlags = (historyValid ? 1u : 0u) | (restarted ? 2u : 0u);
    c.nearZ = m_ws->fd.nearZ; c.farZ = m_ws->fd.farZ;
    c.fadeEnd = static_cast<float>(m_dnFadeSpp); c.fadeStart = static_cast<float>(m_dnFadeSpp / 2);
    const uint32_t gx = (m_ws->w + 7) / 8, gy = (m_ws->h + 7) / 8;

    // ---- Temporal: the history and the first filter input ----
    c.flags = baseFlags;
    c.filterOutUav = uav(m_hist->dnFilter[0]);
    *m_dnCb[0].Current(gfx) = c;
    gfx.BindComputePipelineState(m_dnTemporalPso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_dnCb[0].CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- A-trous: steps 1, 2, 4, 8 over the ping-pong; the last writes the image ----
    gfx.BindComputePipelineState(m_dnAtrousPso, cl);
    for (uint32_t i = 0; i < kDenoisePasses - 1; ++i)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
        c.step = 1u << i;
        c.filterInUav = uav(m_hist->dnFilter[i & 1u]);
        c.filterOutUav = uav(m_hist->dnFilter[(i + 1u) & 1u]);
        c.flags = baseFlags | (i + 2u == kDenoisePasses ? 4u : 0u);
        *m_dnCb[1 + i].Current(gfx) = c;
        gfx.SetComputeRootCBV(kRootCB, m_dnCb[1 + i].CurrentBuffer(gfx), 0, cl);
        gfx.DispatchCompute(gx, gy, 1, cl);
    }
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
    gfx.EndGPUTimestamp(cl, timer);
    m_hist->dnWrite = read;
    m_hist->dnHistoryValid = true;
    if (m_ws == &m_main && m_dnFrames++ % 120 == 0)
        LOG_INFO("[PT_DENOISE] executed=1 samples=%u restarted=%u history=%u fadeSpp=%u width=%u height=%u",
                 samples, restarted ? 1u : 0u, historyValid ? 1u : 0u, m_dnFadeSpp, m_ws->w, m_ws->h);
}

// The camera fog's preview denoiser over the running mean FogCamera just wrote, after the image's:
// FogTemporal into the history, then four FogAtrous passes - the first reading the history, the
// rest the borrowed ping-pong - the last writing the target the composite reads. The fog's samples
// and restart are this frame's (SetFrameData, RecordCameraFog).
void PathTracePass::RecordFogDenoise(RHI::CommandList cl)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    auto srv = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(t)); };
    auto uav = [&](const RHI::Texture& t) { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(t)); };
    History& hist = *m_hist;
    const WorkingSet& ws = *m_ws;
    const uint32_t write = hist.fogDnWrite, read = 1u - hist.fogDnWrite;
    const auto timer = gfx.BeginGPUTimestamp(cl, "PT.FogDenoise");
    if (hist.fogDnOutState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Image(&hist.fogDnOut, hist.fogDnOutState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    hist.fogDnOutState = RHI::ResourceState::UNORDERED_ACCESS;
    // The image's denoiser wrote the ping-pong last, through UAVs.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);

    DenoiseConstants c{};
    c.width = ws.w; c.height = ws.h;
    c.depthIdx = srv(*m_graph.GetPhysicalTexture(m_depth));
    c.fogIdx = srv(hist.fog);
    c.resultUav = uav(hist.fogDnOut);
    c.historyUav = uav(hist.fogDnHistory[read]); c.historyMetaUav = uav(hist.fogDnMeta[read]);
    c.historyOutUav = uav(hist.fogDnHistory[write]); c.historyMetaOutUav = uav(hist.fogDnMeta[write]);
    c.samples = ws.fogSamples;
    const bool historyValid = hist.fogDnHistoryValid && !ws.fd.contentCut && !ws.fd.cameraCut;
    const uint32_t baseFlags = (historyValid ? 1u : 0u) | (ws.fogRestart ? 2u : 0u);
    c.nearZ = ws.fd.nearZ; c.farZ = ws.fd.farZ;
    c.fadeEnd = static_cast<float>(m_dnFadeSpp); c.fadeStart = static_cast<float>(m_dnFadeSpp / 2);
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(c.fogInvViewProj),
                    XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&ws.fd.viewProj))));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(c.fogPrevViewProj), XMMatrixTranspose(XMLoadFloat4x4(&hist.fogDnViewProj)));
    const uint32_t gx = (ws.w + 7) / 8, gy = (ws.h + 7) / 8;

    // ---- Temporal: the history ----
    c.flags = baseFlags;
    *m_fogDnCb[0].Current(gfx) = c;
    gfx.BindComputePipelineState(m_fogDnTemporalPso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_fogDnCb[0].CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- A-trous: steps 1, 2, 4, 8 from the history over the ping-pong; the last writes the target ----
    gfx.BindComputePipelineState(m_fogDnAtrousPso, cl);
    for (uint32_t i = 0; i < kDenoisePasses - 1; ++i)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl);
        c.step = 1u << i;
        c.filterInUav = i == 0 ? c.historyOutUav : uav(hist.dnFilter[(i - 1u) & 1u]);
        c.filterOutUav = uav(hist.dnFilter[i & 1u]);
        c.flags = baseFlags | (i + 2u == kDenoisePasses ? 4u : 0u);
        *m_fogDnCb[1 + i].Current(gfx) = c;
        gfx.SetComputeRootCBV(kRootCB, m_fogDnCb[1 + i].CurrentBuffer(gfx), 0, cl);
        gfx.DispatchCompute(gx, gy, 1, cl);
    }
    // PathTraceFogPass reads the target later in this graph (bindless).
    gfx.PushBarrier(RHI::GPUBarrier::Image(&hist.fogDnOut, hist.fogDnOutState, RHI::ResourceState::SHADER_RESOURCE), cl);
    hist.fogDnOutState = RHI::ResourceState::SHADER_RESOURCE;
    gfx.EndGPUTimestamp(cl, timer);
    hist.fogDnWrite = read;
    hist.fogDnHistoryValid = true;
    hist.fogDnViewProj = ws.fd.viewProj;
    if (m_ws == &m_main && m_fogDnFrames++ % 120 == 0)
        LOG_INFO("[PT_FOG_DENOISE] executed=1 samples=%u restarted=%u history=%u fadeSpp=%u width=%u height=%u",
                 ws.fogSamples, ws.fogRestart ? 1u : 0u, historyValid ? 1u : 0u, m_dnFadeSpp, ws.w, ws.h);
}

// The object-motion probe's entries, counter and readback ring: built with the pass's working set,
// kept until shutdown (a few bytes).
void PathTracePass::EnsureMotionProbe(IGraphicsDevice& gfx)
{
    if (m_motionFailed || !m_layout.IsValid() || m_motion.IsValid()) return;
    auto make = [&](const char* entry, RHI::PipelineState& pso)
    {
        auto shader = ShaderDiskCache::LoadOrCompile("shaders/PathTraceDenoise.cs.hlsl", entry, RHI::ShaderStage::CS, {}, "PathTraceDenoise");
        return !shader.empty() && ComputePipelineBatch::Create(gfx, m_layout, &shader, 1, &pso);
    };
    bool ok = make("MotionClear", m_motionClearPso) && make("MotionProbe", m_motionProbePso);
    if (ok && !m_motionCb.IsValid()) ok = m_motionCb.Create(gfx, "PT.MotionConstants");
    RHI::GPUBufferDesc bd{};
    bd.size = 2 * sizeof(uint32_t); bd.stride = sizeof(uint32_t);   // moved, time-reading
    bd.usage = RHI::Usage::DEFAULT; bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    if (ok) ok = gfx.CreateBuffer(bd, m_motion);
    bd.usage = RHI::Usage::READBACK; bd.bindFlags = RHI::BindFlag::NONE;
    for (uint32_t i = 0; ok && i < RHI::kFramesInFlight; ++i)
        ok = gfx.CreateBuffer(bd, m_motionReadback[i]) &&
             (m_motionMapped[i] = static_cast<const uint32_t*>(gfx.MapBuffer(m_motionReadback[i]))) != nullptr;
    if (!ok)
    {
        m_motionFailed = true;
        LOG_ERROR("PathTracePass: object-motion probe failed");
        return;
    }
    m_motionState = RHI::ResourceState::UNDEFINED;
}

bool PathTracePass::ReadObjectMotion(IGraphicsDevice& gfx)
{
    const auto slot = gfx.GetFrameIndex();
    if (!m_motionPrimed[slot] || !m_motionMapped[slot]) return false;
    m_motionPrimed[slot] = false;   // one verdict per measured frame
    m_motionPixels = m_motionMapped[slot][0];
    m_timePixels = m_motionMapped[slot][1];
    const bool moved = (m_motionPixels != 0 && m_motionCameraStill[slot]) ||
                       (m_timePixels != 0 && m_motionClockRan[slot]);
    if (moved && StabilityLog())
        LOG_INFO("[PT_MOTION] pixels=%u timePixels=%u", m_motionPixels, m_timePixels);
    return moved;
}

// Counts the pixels that moved on their own this frame into m_motion and copies the count to this
// frame slot's readback. GBuffer primary only (camera rays leave no motion vectors to read).
void PathTracePass::RecordMotionProbe(RHI::CommandList cl)
{
    if (!m_motion.IsValid() || !m_motionProbePso.IsValid() || !m_velocity.IsValid() || (m_flags & 512u)) return;
    IGraphicsDevice& gfx = cl.GetDevice();
    const auto slot = gfx.GetFrameIndex();
    auto srv = [&](RG::RGTextureHandle h)
    { return gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(*m_graph.GetPhysicalTexture(h))); };
    if (m_motionState != RHI::ResourceState::UNORDERED_ACCESS)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_motion, m_motionState, RHI::ResourceState::UNORDERED_ACCESS), cl);
    m_motionState = RHI::ResourceState::UNORDERED_ACCESS;
    DenoiseConstants c{};
    c.width = m_ws->w; c.height = m_ws->h;
    c.depthIdx = srv(m_depth); c.velocityIdx = srv(m_velocity);
    c.motionUav = gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetBufferUAVGpuHandle(m_motion));
    c.nearZ = m_ws->fd.nearZ; c.farZ = m_ws->fd.farZ;
    c.normalIdx = srv(m_normal);
    c.materialsIdx = m_ws->fd.materialsIdx;
    *m_motionCb.Current(gfx) = c;
    gfx.BindComputePipelineState(m_motionClearPso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_motionCb.CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute(1, 1, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_motion), cl);
    gfx.BindComputePipelineState(m_motionProbePso, cl);
    gfx.SetComputeRootCBV(kRootCB, m_motionCb.CurrentBuffer(gfx), 0, cl);
    gfx.DispatchCompute((m_ws->w + 7) / 8, (m_ws->h + 7) / 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_motion, RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::COPY_SRC), cl);
    m_motionState = RHI::ResourceState::COPY_SRC;
    gfx.CopyBuffer(m_motion, m_motionReadback[slot], 2 * sizeof(uint32_t), cl);
    m_motionPrimed[slot] = true;
    m_motionCameraStill[slot] = !m_ws->cameraMoved && !m_ws->fd.cameraCut;
    m_motionClockRan[slot] = m_ws->fd.worldClockRuns;
}

void PathTracePass::OnService(RG::ServiceContext& c)
{
    const ServiceRequest& r = m_request;
    if (c.stage == RG::ServiceStage::StaticGeometry)
    {
        OnStaticGeometryComplete();
        return;
    }
    if (c.stage == RG::ServiceStage::DirectLight)
    {
        SetDirectLight(r.directLight);   // a change remakes the core
        return;
    }
    for (size_t i = 0; i < m_retiredNrd.size();)
    {
        if (--m_retiredNrd[i].second == 0) m_retiredNrd.erase(m_retiredNrd.begin() + static_cast<ptrdiff_t>(i));
        else ++i;
    }
    SetAreaSettings(r.areaMode, r.areaMotion, r.areaHistory, r.areaSpatialK, r.areaSpatialRadius, r.areaReconnect,
                    r.areaMotionPinned);
    SetRayReconstruction(r.rrRuns);
    SetRealtimePreset(r.realtime);
    SetSettings(r.maxBounces, r.reference, r.bouncesPinned, r.referencePinned);
    SetCameraSettings(r.cameraPrimary, r.filmJitter, r.primaryRayCone, r.lensRadius, r.focusDistance);
    SetBackend(r.backend);
    SetFrontReflection(r.frontReflection);
    SetNprLighting(r.nprLighting);
    SetDenoise(r.denoise, r.denoiseFadeSpp);
    EnsureResources(r.wanted);
}
