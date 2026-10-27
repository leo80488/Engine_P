#include "System/CommandLine.h"
#include "RenderGraph/RenderPass/RTReflectionPass.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/MeshDescriptorHeap.h"   // kMaxBuffers - bindless table size
// DDGI::kMaxVolumes - this pass declares its OWN root signature with three
// DDGI SRV arrays, so their range widths must track the engine-wide ceiling:
// a drifted width silently mismatches the shader's DDGI_MAX_VOLUMES-wide
// declarations.
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/ReflectionProbeTypes.h"   // the probe stride
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/NrdDenoiser.h"          // NRD (REBLUR_SPECULAR) backend
#include "Graphics/TransientAliasArena.h"  // shared transient alias heap
#include "RenderGraph/PassTextureNames.h"
#include "System/Check.h"
#include "System/Log.h"

#include <chrono>
#include <vector>
#include <string>
#include <iterator>
#include <cstring>

namespace
{
// Shader compiles go through the shared disk cache (ShaderDiskCache:
// ShaderLibrary's .ishdr cache in shaders/shader_cache_dxil/, keyed on path +
// entry + stage + defines + compile flags, invalidated by the mtime of the
// source or any of its transitive #includes), so a warm boot pays no DXC run.
std::vector<uint8_t> CompileShaderFile(const std::string& path,
                                       const std::string& entry)
{
    std::vector<DxcCompiler::Define> defines;
    if (Sys::CommandLine::Get().Has("-rtgi_reference"))
        defines.push_back({"DDGI_FUSED_SKY_SAMPLE", "0"});
    return ShaderDiskCache::LoadOrCompile(path, entry, RHI::ShaderStage::CS, defines,
                                          "RTReflectionPass");
}

// Root parameter indices - MUST stay in sync with the register map in
// shaders/HybridReflCommon.hlsli (the single source of truth).
constexpr RHI::Slots::CmpParam kCB                 { 0 };   // b0
constexpr RHI::Slots::CmpParam kTLAS               { 1 };   // t0  (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kInstances          { 2 };   // t1  (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kSRV_Depth          { 3 };   // t2
constexpr RHI::Slots::CmpParam kSRV_Normal         { 4 };   // t3
constexpr RHI::Slots::CmpParam kSRV_Surface        { 5 };   // t4
constexpr RHI::Slots::CmpParam kSRV_Velocity       { 6 };   // t5
constexpr RHI::Slots::CmpParam kSRV_Lights         { 7 };   // t6
constexpr RHI::Slots::CmpParam kSRV_DDGIVolumes    { 8 };   // t7
constexpr RHI::Slots::CmpParam kSRV_DDGISH         { 9 };   // t0 space4, DDGI::kMaxVolumes wide
constexpr RHI::Slots::CmpParam kSRV_DDGIDepth      { 10 };  // t0 space5, DDGI::kMaxVolumes wide
constexpr RHI::Slots::CmpParam kSRV_DDGIProbe      { 11 };  // t0 space6, DDGI::kMaxVolumes wide
constexpr RHI::Slots::CmpParam kSRV_PrevHDR        { 12 };  // t20
constexpr RHI::Slots::CmpParam kSRV_PrevDepth      { 13 };  // t21
constexpr RHI::Slots::CmpParam kSRV_SkySH          { 14 };  // t22
constexpr RHI::Slots::CmpParam kSRV_SkyCube        { 15 };  // t23
constexpr RHI::Slots::CmpParam kSRV_SkyIrr         { 16 };  // t24
// t25 / t26 are unused: the probe buffer is opened from the heap through
// HybridReflConstants.probeBufferIdx.
constexpr RHI::Slots::CmpParam kSRV_DepthHier      { 17 };  // t27 (2-ch Hi-Z pyramid)
constexpr RHI::Slots::CmpParam kSRV_RawColor       { 18 };  // t28 (pack/resolve read)
constexpr RHI::Slots::CmpParam kSRV_HitDist        { 19 };  // t29 (pack read)
constexpr RHI::Slots::CmpParam kSRV_NrdSpecOut     { 20 };  // t30 (resolve read)
constexpr RHI::Slots::CmpParam kSRV_HitCounterHist { 21 };  // t31 (classify read, prev frame)
constexpr RHI::Slots::CmpParam kUAV_RawColor       { 22 };  // u0
constexpr RHI::Slots::CmpParam kUAV_HitDist        { 23 };  // u1
constexpr RHI::Slots::CmpParam kUAV_SWRayList      { 24 };  // u2
constexpr RHI::Slots::CmpParam kUAV_HWRayList      { 25 };  // u3
constexpr RHI::Slots::CmpParam kUAV_RayCounter     { 26 };  // u4
constexpr RHI::Slots::CmpParam kUAV_IndirectArgs   { 27 };  // u5
constexpr RHI::Slots::CmpParam kUAV_HitCounter     { 28 };  // u6
constexpr RHI::Slots::CmpParam kUAV_Denoised       { 29 };  // u7 (-> Lighting.ps via LightCB.rtReflectionIdx)
constexpr RHI::Slots::CmpParam kUAV_DepthHistory   { 30 };  // u8
constexpr RHI::Slots::CmpParam kUAV_NrdNormalRough { 31 };  // u9
constexpr RHI::Slots::CmpParam kUAV_NrdViewZ       { 32 };  // u10
constexpr RHI::Slots::CmpParam kUAV_NrdMV          { 33 };  // u11
constexpr RHI::Slots::CmpParam kUAV_NrdSpecIn      { 34 };  // u12
constexpr RHI::Slots::CmpParam kBindlessBuffers    { 35 };  // t0.. space1
constexpr RHI::Slots::CmpParam kBindlessTex        { 36 };  // t0.. space2
constexpr RHI::Slots::CmpParam kAlphaRecords       { 37 };  // t32 (root SRV, raw VA) - RTAlphaRecord
constexpr uint32_t kNumRootParams      = 38;

// g_IndirectArgs layout (uints): [0..2] SW dispatch args, [3] pad,
//                                [4..6] HW dispatch args, [7] pad.
constexpr uint64_t kArgsSWByteOffset = 0;
constexpr uint64_t kArgsHWByteOffset = 4 * sizeof(uint32_t);

// Local transition helper for pass-owned textures (graph doesn't track them).
void TransitionTex(IGraphicsDevice& gfx,
                   const RHI::Texture& tex,
                   RHI::ResourceState& state,
                   RHI::ResourceState to,
                   RHI::CommandList cl)
{
    if (state == to || !tex.IsValid()) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}
} // namespace

// ==== Lifecycle ====

RTReflectionPass::RTReflectionPass(RG::RGTextureHandle normal,
                                   RG::RGTextureHandle surface,
                                   RG::RGTextureHandle depth,
                                   RG::RGTextureHandle velocity,
                                   RG::RGTextureHandle traceDepth,
                                   Variant variant)
    : m_normalH(normal), m_surfaceH(surface), m_depthH(depth),
      m_velocityH(velocity), m_traceDepthH(traceDepth), m_variant(variant)
{
    const bool front = (variant == Variant::TranslucentFront);
    // Every string that can identify this instance in a capture, a profiler
    // region or a log line is prefixed per variant - telling arena residents
    // apart (TransientAliasArena.h) depends on these names, and
    // GetGPUTimestampResult is first-match-wins.
    m_dbgPrefix      = front ? "RTRF." : "RTR.";
    m_ownsHiZ        = !front;
    m_swTierPossible = !front;
    m_bypassDenoiser = front;
    // HybridReflCommon.hlsli rayMask. Both instances trace 0x08, the GBuffer's
    // surfaces (DDGISceneAS instanceMask: every main-pass instance but the
    // forward-translucent ones). Do not trace 0x01 from the opaque instance: it
    // would commit glass and alpha-blend posters / decals as SOLID - the blend
    // accept is unconditional for reflection rays and the hit shading has no
    // opacity - so a window in a floor reflection becomes a bluish mirror and a
    // mostly-transparent decal a black quad. The path tracer's rays pass through
    // them the same way.
    m_rayMask        = 0x08u;
    static constexpr const char* kOpaqueStages[7] = {
        "RTR.ClearCounters", "RTR.Classify", "RTR.IntersectSW", "RTR.IntersectHW",
        "RTR.NRDPack", "RTR.NRDDenoise", "RTR.Resolve" };
    static constexpr const char* kFrontStages[7] = {
        "RTRF.ClearCounters", "RTRF.Classify", "RTRF.IntersectSW", "RTRF.IntersectHW",
        "RTRF.NRDPack", "RTRF.NRDDenoise", "RTRF.Resolve" };
    for (int i = 0; i < 7; ++i)
        m_stageNames[i] = front ? kFrontStages[i] : kOpaqueStages[i];
}

// The denoised output (RGBA16F, the render size) as this instance's import -
// the graph reads the member at each Execute and transitions it (Graph
// tracking): UNORDERED_ACCESS in this pass's Phase A, SHADER_RESOURCE in each
// reader's. The member is the bound view's output (SyncImports).
void RTReflectionPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_denoisedH = d.Import(m_variant == Variant::TranslucentFront ? RG::PassTex::kRTReflectionFront
                                                                  : RG::PassTex::kRTReflection,
                           { RHI::Format::R16G16B16A16_FLOAT, 0, 0, false, true },
                           { &m_denoisedImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
}

void RTReflectionPass::Setup(RG::RenderGraphBuilder& b)
{
    // GBuffer inputs - the graph transitions them to SRV-readable states before
    // this pass (depth -> DEPTH_READ_SRV, colors -> SHADER_RESOURCE). Every output
    // but the denoised import is pass-owned; those barriers are manual in Execute.
    b.ReadSRV(m_normalH);
    b.ReadSRV(m_surfaceH);
    b.ReadSRV(m_depthH);
    b.ReadSRV(m_velocityH);
    // Grass-free depth snapshot (SSRTraceDepthSnapshot pass, pre-GrassPass) -
    // the Hi-Z build reads it; DEPTH_READ_SRV is CS-readable.
    b.ReadSRV(m_traceDepthH);
    // Last frame's ReSTIR GI (opaque instance only; SetRestirGIResult) - this
    // pass runs before RTGIPass writes this frame's, on purpose: ReadHistory,
    // so the graph's DAG does not call it a stale read.
    if (m_giResultH.IsValid()) b.ReadHistory(m_giResultH);
    // The pass textures it reads and the one it writes (the graph transitions
    // the denoised output). The opaque instance's screen tier traces SSGI's Hi-Z
    // (SetExternalDepthHierarchy; the front never runs that tier) and both reuse
    // LAST frame's SSR scene pyramid; the analytic hits sample the sky cube.
    if (m_variant != Variant::TranslucentFront) b.ReadSRV(b.GetTexture(RG::PassTex::kSSGIHiZ));
    b.ReadHistory(b.GetTexture(RG::PassTex::kSSRScenePyramid));
    b.ReadSRV(b.GetTexture(RG::PassTex::kSkySpecularCube));
    b.WriteUAV(m_denoisedH);
}

void RTReflectionPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    if (!gfx.GetCapabilities().inlineRaytracing)
    {
        LOG_INFO("RTReflectionPass: device does not support DXR — pass disabled");
        m_ready = false;
        return;
    }

    if (!BuildRootSignature(gfx)) return;
    if (!CreatePipelines(gfx))    return;
    // The two indirect intersects use the device's dispatch signature
    // (IGraphicsDevice::DispatchComputeIndirect).

    if (!m_cb.Create(gfx, (m_variant == Variant::TranslucentFront)
                              ? "RTReflectionFront.CB" : "RTReflection.CB"))
    {
        LOG_ERROR("%s: CB creation failed", GetName());
        return;
    }

    m_bindlessTexTable = gfx.GetBindlessTextureTableGpuHandle();

    // SW tier: owned Hi-Z pyramid builder (the class the water SSR chain uses
    // too; it binds the SHARED compute root sig, so it must run BEFORE this
    // pass's own root signature is set each frame).
    // The TranslucentFront variant never runs the SW tier (screenOk is false
    // by construction), so it builds no pyramid - ~11 MB at 1080p saved. A
    // secondary view's builder is its own, made with its working set.
    if (m_ownsHiZ)
    {
        m_main.hiZ = std::make_unique<SSRDepthHierarchyPass>();
        m_main.hiZ->Init(gfx);
    }

    m_ready = true;
    if (m_variant == Variant::TranslucentFront)
        LOG_INFO("RTReflectionFrontLayerPass: initialised (translucent front-layer "
                 "reflections — RayQuery from the TranslucentFront stamp, mask 0x08, "
                 "denoiser bypassed)");
    else
        LOG_INFO("RTReflectionPass: initialised (FFX-HSR hybrid reflections — "
                 "classify → SW Hi-Z intersect → HW RayQuery fallback → NRD)");
}

void RTReflectionPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_ready) return;
    const bool hadHWTier = m_intersectHWPSO.IsValid();   // in use: rebuilt now
    m_hwFailed = false;
    for (RHI::PipelineState* pso : { &m_classifyPSO, &m_clearCountersPSO, &m_prepareArgsSWPSO, &m_prepareArgsHWPSO,
                                     &m_intersectSWPSO, &m_intersectHWPSO, &m_nrdPackPSO, &m_nrdResolvePSO })
        gfx.DestroyPipelineState(*pso);
    if (!CreatePipelines(gfx))
    {
        LOG_ERROR("RTReflectionPass: shader reload failed — pass disabled");
        m_ready = false;
        return;
    }
    if (hadHWTier) EnsureHWTier();
    // The main view's Hi-Z builder; a secondary view's keeps its pipelines until the view is made again.
    if (m_main.hiZ) m_main.hiZ->ReloadShaders(gfx);
    ResetHistory();   // don't blend a stale accumulator post-reload
}

// ==== Root signature - see the map in HybridReflCommon.hlsli ====

bool RTReflectionPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    RHI::PipelineLayoutBuilder b(GetName());
    b.RootCBV(kCB, 0)
     .RootSRV(kTLAS, 0)            // t0 - TLAS
     .RootSRV(kInstances, 1)       // t1 - instance data by raw VA
     // Per-instance alpha-cutout records (40 B) - read per traversal CANDIDATE
     // by IntersectHW. Same InstanceID() index space as kInstances.
     .RootSRV(kAlphaRecords, 32)   // t32
     .Table(kSRV_Depth,          { RHI::SrvRange(2) })
     .Table(kSRV_Normal,         { RHI::SrvRange(3) })
     .Table(kSRV_Surface,        { RHI::SrvRange(4) })
     .Table(kSRV_Velocity,       { RHI::SrvRange(5) })
     .Table(kSRV_Lights,         { RHI::SrvRange(6) })
     .Table(kSRV_DDGIVolumes,    { RHI::SrvRange(7) })
     // Each DDGI array owns a PRIVATE register space: in one space the arrays
     // would run into each other past a fixed width (dxc errors), so one space each
     // leaves DDGI_MAX_VOLUMES free. Costs no extra root DWORDs - a descriptor
     // table is 1 DWORD regardless of range size.
     .Table(kSRV_DDGISH,         { RHI::SrvRange(0, DDGI::kMaxVolumes, 4) })
     .Table(kSRV_DDGIDepth,      { RHI::SrvRange(0, DDGI::kMaxVolumes, 5) })
     .Table(kSRV_DDGIProbe,      { RHI::SrvRange(0, DDGI::kMaxVolumes, 6) })
     .Table(kSRV_PrevHDR,        { RHI::SrvRange(20) })
     .Table(kSRV_PrevDepth,      { RHI::SrvRange(21) })
     .Table(kSRV_SkySH,          { RHI::SrvRange(22) })
     .Table(kSRV_SkyCube,        { RHI::SrvRange(23) })
     .Table(kSRV_SkyIrr,         { RHI::SrvRange(24) })
     .Table(kSRV_DepthHier,      { RHI::SrvRange(27) })
     .Table(kSRV_RawColor,       { RHI::SrvRange(28) })
     .Table(kSRV_HitDist,        { RHI::SrvRange(29) })
     .Table(kSRV_NrdSpecOut,     { RHI::SrvRange(30) })
     .Table(kSRV_HitCounterHist, { RHI::SrvRange(31) })
     .Table(kUAV_RawColor,       { RHI::UavRange(0) })
     .Table(kUAV_HitDist,        { RHI::UavRange(1) })
     .Table(kUAV_SWRayList,      { RHI::UavRange(2) })
     .Table(kUAV_HWRayList,      { RHI::UavRange(3) })
     .Table(kUAV_RayCounter,     { RHI::UavRange(4) })
     .Table(kUAV_IndirectArgs,   { RHI::UavRange(5) })
     .Table(kUAV_HitCounter,     { RHI::UavRange(6) })
     .Table(kUAV_Denoised,       { RHI::UavRange(7) })
     .Table(kUAV_DepthHistory,   { RHI::UavRange(8) })
     .Table(kUAV_NrdNormalRough, { RHI::UavRange(9) })
     .Table(kUAV_NrdViewZ,       { RHI::UavRange(10) })
     .Table(kUAV_NrdMV,          { RHI::UavRange(11) })
     .Table(kUAV_NrdSpecIn,      { RHI::UavRange(12) })
     // Bindless ByteAddressBuffer table (t0.. space1) - VB/IB fetch for hit
     // attributes, same convention / size as DDGIPass - and the bindless
     // texture table (t0.. space2) - material textures at HW hits.
     .Table(kBindlessBuffers,    { RHI::SrvRange(0, MeshDescriptorHeap::kMaxBuffers, 1) })
     .Table(kBindlessTex,        { RHI::SrvRange(0, GraphicsDX12::kMaxBindlessTextures, 2) });
    // s0 - linear CLAMP (pyramid/history), s1 - linear WRAP (DDGI/materials).
    // MaxLOD stays FLT_MAX (SamplerDesc's default): a zero MaxLOD clamps every
    // SampleLevel to mip 0, silently disabling the roughness-matched
    // prev-pyramid / probe / sky-cube prefilter mips this pass depends on.
    RHI::SamplerDesc linearClamp{};
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = RHI::TextureAddressMode::CLAMP;
    linearClamp.maxAnisotropy = 1;
    b.Sampler(linearClamp, 0);
    RHI::SamplerDesc linearWrap{};
    linearWrap.maxAnisotropy = 1;
    b.Sampler(linearWrap, 1);
    // HEAP_DIRECTLY_INDEXED so HybridReflIntersectSW/HW can sample per-probe
    // reflection cubemaps via ResourceDescriptorHeap[...].
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("%s: pipeline layout failed", GetName());
        return false;
    }
    return true;
}

bool RTReflectionPass::BuildCS(IGraphicsDevice& gfx, const std::string& path, const std::string& entry,
                               RHI::PipelineState& outPSO)
{
    const auto dxil = CompileShaderFile(path, entry);
    if (dxil.empty()) return false;
    RHI::Shader cs;
    RHI::PipelineStateDesc pd{};
    pd.cs = &cs; pd.layout = &m_layout;
    const bool ok = gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs) &&
                    gfx.CreatePipelineState(pd, outPSO);
    gfx.DestroyShader(cs);   // the PSO keeps its own copy of the DXIL
    if (!ok)
        LOG_ERROR("RTReflectionPass: pipeline failed for %s:%s", path.c_str(), entry.c_str());
    return ok;
}

// Every pipeline but the hardware tier's (EnsureHWTier): the screen-space
// tier, the classify / args / pack / resolve stages both tiers share.
bool RTReflectionPass::CreatePipelines(IGraphicsDevice& gfx)
{
    if (!BuildCS(gfx, "shaders/HybridReflClassify.cs.hlsl",    "main",        m_classifyPSO))      return false;
    if (!BuildCS(gfx, "shaders/HybridReflPrepareArgs.cs.hlsl", "CSMainClear", m_clearCountersPSO)) return false;
    if (!BuildCS(gfx, "shaders/HybridReflPrepareArgs.cs.hlsl", "CSMainSW",    m_prepareArgsSWPSO)) return false;
    if (!BuildCS(gfx, "shaders/HybridReflPrepareArgs.cs.hlsl", "CSMainHW",    m_prepareArgsHWPSO)) return false;
    if (!BuildCS(gfx, "shaders/HybridReflIntersectSW.cs.hlsl", "main",        m_intersectSWPSO))   return false;
    if (!BuildCS(gfx, "shaders/HybridReflNRDPack.cs.hlsl",     "main",        m_nrdPackPSO))       return false;
    if (!BuildCS(gfx, "shaders/HybridReflNRDResolve.cs.hlsl",  "main",        m_nrdResolvePSO))    return false;
    return true;
}

bool RTReflectionPass::EnsureHWTier()
{
    if (m_intersectHWPSO.IsValid()) return true;
    if (!m_ready || m_hwFailed || !m_gfx) return false;
    const auto start = std::chrono::steady_clock::now();
    if (!BuildCS(*m_gfx, "shaders/HybridReflIntersectHW.cs.hlsl", "main", m_intersectHWPSO))
    {
        m_hwFailed = true;
        LOG_ERROR("%s: the hardware tier could not be built - RT reflections stay off", GetName());
        return false;
    }
    LOG_INFO("%s: hardware tier built on first use (%.0f ms)", GetName(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

// ==== Resolution-dependent resources ====
// OnCompile is the serial, race-free hook: Execute bodies are recorded in
// parallel, and descriptor allocation there races. A secondary view's working
// set is made in its segment, before its graph records (EnsureViewResources).

void RTReflectionPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this, m_variant == Variant::TranslucentFront ? "RTReflectionFront"
                                                                                   : "RTReflection");
    if (view.Slot() == 0)
        m_ws = &m_main;
    else
    {
        ViewWork& work = view.Data<ViewWork>(this);
        work.owner = this;
        m_ws = &work.ws;
    }
    SyncImports();
}

void RTReflectionPass::History::Release(IGraphicsDevice& gfx)
{
    for (int i = 0; i < 2; ++i)
    {
        if (depthHist[i].IsValid())  gfx.DestroyTexture(depthHist[i]);
        if (hitCounter[i].IsValid()) gfx.DestroyTexture(hitCounter[i]);
        depthHist[i] = {}; hitCounter[i] = {};
    }
}

void RTReflectionPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    DestroyWorkingSet(gfx, ws);
    if (ws.hiZ) ws.hiZ->Shutdown();
    if (owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd), RHI::kFramesInFlight + 1u);
    ws = WorkingSet{};
}

void RTReflectionPass::OnService(RG::ServiceContext& c)
{
    if (c.stage == RG::ServiceStage::RTReflectionTier)
    {
        if (m_request.hwTier) EnsureHWTier();
        for (size_t i = 0; i < m_retiredNrd.size();)
        {
            if (--m_retiredNrd[i].second == 0)
                m_retiredNrd.erase(m_retiredNrd.begin() + static_cast<ptrdiff_t>(i));
            else
                ++i;
        }
    }
    else if (c.stage == RG::ServiceStage::WorkingSets)
        SetRayReconstruction(m_request.rr);
    SyncImports();
}

void RTReflectionPass::EnsureViewResources(uint32_t w, uint32_t h, bool wanted)
{
    if (m_ws == &m_main || !m_ready || !m_gfx || !m_hist) return;
    if (wanted)
        BuildWorkingSet(*m_gfx, w, h, /*mayStall=*/false);
    else if (m_ws->w != 0)
    {
        DestroyResolutionResources(*m_gfx);
        if (m_ws->hiZ) m_ws->hiZ->ReleaseTextures();
        RetireNrd();
        m_ws->w = m_ws->h = m_ws->tileX = m_ws->tileY = 0;
    }
    SyncImports();
}

void RTReflectionPass::RetireNrd()
{
    if (m_ws->nrd) m_retiredNrd.emplace_back(std::move(m_ws->nrd), RHI::kFramesInFlight + 1u);
}

void RTReflectionPass::DestroyResolutionResources(IGraphicsDevice& gfx)
{
    if (m_ws == &m_main)
    {
        for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
        {
            if (m_rayReadbackMapped[i]) gfx.UnmapBuffer(m_rayReadback[i]);
            if (m_rayReadback[i].IsValid()) gfx.DestroyBuffer(m_rayReadback[i]);
            m_rayReadback[i] = {};
            m_rayReadbackMapped[i] = nullptr;
            m_rayReadbackPrimed[i] = false;
        }
        if (m_rrHitDist.IsValid()) gfx.DestroyTexture(m_rrHitDist);
        m_rrHitDist = {};
        m_rrHitDistWritten = false;
    }
    DestroyWorkingSet(gfx, *m_ws);

    if (m_hist) m_hist->Release(gfx);   // the history last
}

void RTReflectionPass::DestroyWorkingSet(IGraphicsDevice& gfx, WorkingSet& ws)
{
    for (RHI::GPUBuffer* buf : { &ws.swRayList, &ws.hwRayList, &ws.rayCounter, &ws.indirectArgs })
    {
        if (buf->IsValid()) gfx.DestroyBuffer(*buf);
        *buf = {};
    }
    DestroyTransientTextures(gfx, ws);
    if (ws.denoised.IsValid()) gfx.DestroyTexture(ws.denoised);
    ws.denoised = {};
}

void RTReflectionPass::DestroyTransientTextures(IGraphicsDevice& gfx, WorkingSet& ws)
{
    for (RHI::Texture* tex : { &ws.rawColor, &ws.hitDist, &ws.nrdNormalRough, &ws.nrdViewZ, &ws.nrdMV,
                               &ws.nrdSpecIn, &ws.nrdSpecOut })
    {
        if (tex->IsValid()) gfx.DestroyTexture(*tex);
        *tex = {};
    }
    ws.placedMask = 0;
}

// The 7 frame-transient scratch textures. Placed into the shared alias arena
// when available (they alias XeGTAO/VolFog/Bloom transients - GPU-timeline
// disjointness argument in TransientAliasArena.h); committed fallback per
// texture on any failure. The 7 co-live within the RTR pass window, so they
// get DISTINCT offsets - the sharing is with the OTHER groups, all of which
// pack from offset 0 in the same heap. A secondary view's are placed in the views'
// shared scratch heap (WorkArena), by the same argument: its segment runs the same
// passes in the same order, and views record one after another.
void RTReflectionPass::CreateTransientTextures(IGraphicsDevice& gfx, WorkingSet& ws, TransientAliasArena* arena)
{
    RHI::TextureDesc td{};
    td.width      = ws.w;
    td.height     = ws.h;
    td.depth      = 1;
    td.mipLevels = 1;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    struct T { const char* tag; RHI::Format fmt; RHI::Texture* tex; RHI::ResourceState* st; };
    const T transients[] = {
        { "rawColor",    RHI::Format::R16G16B16A16_FLOAT, &ws.rawColor,       &ws.rawColorState },
        { "hitDist",     RHI::Format::R16_FLOAT,          &ws.hitDist,        &ws.hitDistState },
        { "normalRough", RHI::Format::R10G10B10A2_UNORM,  &ws.nrdNormalRough, &ws.nrdNormalRoughState },
        { "viewZ",       RHI::Format::R32_FLOAT,          &ws.nrdViewZ,       &ws.nrdViewZState },
        { "mv",          RHI::Format::R16G16_FLOAT,       &ws.nrdMV,          &ws.nrdMVState },
        { "specIn",      RHI::Format::R16G16B16A16_FLOAT, &ws.nrdSpecIn,      &ws.nrdSpecInState },
        { "specOut",     RHI::Format::R16G16B16A16_FLOAT, &ws.nrdSpecOut,     &ws.nrdSpecOutState },
    };
    constexpr int kCount = int(sizeof(transients) / sizeof(transients[0]));
    static_assert(kCount <= 8, "WorkingSet::placedMask is 8 bits");

    bool placed = false;   // in the arena's heap (through IGraphicsDevice)
    uint64_t offsets[kCount]{};
    if (arena)
    {
        uint64_t cursor = 0;
        bool ok = true;
        for (int i = 0; i < kCount; ++i)
        {
            RHI::TextureDesc d = td;
            d.format = transients[i].fmt;
            uint64_t sz = 0, al = 0;
            if (!gfx.GetTextureAllocationInfo(d, sz, al)) { ok = false; break; }
            offsets[i] = TransientAliasArena::AlignUp(cursor, al);
            cursor     = offsets[i] + sz;
        }
        if (ok)
            placed = gfx.EnsureAliasArena(*arena, cursor);
    }

    ws.placedMask = 0;
    for (int i = 0; i < kCount; ++i)
    {
        RHI::TextureDesc d = td;
        d.format = transients[i].fmt;
        // `dbg` outlives the create call, which copies the string. Without it these
        // are "Unnamed" in a RenderDoc capture (why that matters: the denoised
        // output's create in BuildWorkingSet).
        const std::string dbg = std::string(m_dbgPrefix) + transients[i].tag;
        d.debugName = dbg.c_str();

        bool created = false;
        if (placed && gfx.CreateTexturePlaced(d, *arena, offsets[i], *transients[i].tex))
        {
            created = true;
            ws.placedMask |= uint8_t(1u << i);
        }
        if (!created && !gfx.CreateTexture(d, *transients[i].tex))
            LOG_ERROR("RTReflectionPass: %s texture creation failed", transients[i].tag);
        *transients[i].st = RHI::ResourceState::SHADER_RESOURCE;
    }
    if (arena)
        ws.placedEpoch = arena->Epoch();
}

// Main thread, PRE-GRAPH only (Renderer calls this right before wiring the
// pass's frame data). Rebuilds the bound view's transients when another arena
// resident grew the heap after our OnCompile - the old textures sit on the
// retired heap (kept alive by their own placedHeap refs, so this is correctness-
// neutral); rebuilding restores the actual byte sharing.
TransientAliasArena* RTReflectionPass::WorkArena() const
{
    return m_aliasArena && m_ws != &m_main ? m_aliasArena->ViewTwin() : m_aliasArena;
}

void RTReflectionPass::EnsureTransientsCurrent(IGraphicsDevice& gfx)
{
    TransientAliasArena* arena = WorkArena();
    if (!arena || !m_ws->placedMask) return;
    if (m_ws->placedEpoch == arena->Epoch()) return;
    if (m_ws->w == 0 || m_ws->h == 0) return;
    DestroyTransientTextures(gfx, *m_ws);
    CreateTransientTextures(gfx, *m_ws, arena);
}

void RTReflectionPass::OnCompile(IGraphicsDevice& gfx)
{
    if (!m_ready) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "%s: no view bound (PassSet::SetView before its registration)",
                           GetName()))
        return;
    BuildWorkingSet(gfx, gfx.GetRenderWidth(), gfx.GetRenderHeight(), /*mayStall=*/true);
    SyncImports();
}

void RTReflectionPass::BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall)
{
    WorkingSet& ws = *m_ws;
    History& hist = *m_hist;
    if (w == 0 || h == 0) return;
    // Early-out only when the FULL resource set exists - a partial creation
    // failure must be retried on the next build, not frozen forever.
    if (w == ws.w && h == ws.h && AllResourcesValid())
        return;

    DestroyResolutionResources(gfx);
    ws.w = w;
    ws.h = h;
    ws.tileX = (w + 7u) / 8u;
    ws.tileY = (h + 7u) / 8u;
    hist.writeIdx = 0;
    hist.reset    = true;

    // SW-tier Hi-Z pyramid - created in UNORDERED_ACCESS (state contract in
    // SSRDepthHierarchyPass.h); Execute flips it UAV -> SRV -> UAV per frame.
    // A secondary view's builder is made with its first working set.
    if (m_ownsHiZ && !ws.hiZ)
    {
        ws.hiZ = std::make_unique<SSRDepthHierarchyPass>();
        ws.hiZ->Init(gfx);
    }
    if (ws.hiZ)
    {
        ws.hiZ->EnsureTexture(w, h);
        ws.hiZTexState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    {
        // Ray lists sized for the worst case (every pixel routed to one list).
        RHI::GPUBufferDesc bd{};
        bd.size       = uint64_t(w) * uint64_t(h) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        // The SW ray list only exists where the SW tier can run; the front
        // instance binds the HW list's UAV (identical desc) in its slot.
        if (m_swTierPossible && !gfx.CreateBuffer(bd, ws.swRayList))
            LOG_ERROR("%s: SW ray list creation failed", GetName());
        if (!gfx.CreateBuffer(bd, ws.hwRayList))
            LOG_ERROR("%s: HW ray list creation failed", GetName());

        bd.size = 8 * sizeof(uint32_t);
        if (!gfx.CreateBuffer(bd, ws.rayCounter))
            LOG_ERROR("RTReflectionPass: ray counter creation failed");
        if (!gfx.CreateBuffer(bd, ws.indirectArgs))
            LOG_ERROR("RTReflectionPass: indirect args creation failed");
#ifdef WITH_HARNESS
        if (m_ws == &m_main && Sys::CommandLine::Get().Has("-rt_raycounts"))
        {
            bd.usage = RHI::Usage::READBACK;
            bd.bindFlags = RHI::BindFlag::NONE;
            bd.miscFlags = RHI::ResourceMiscFlag::NONE;
            for (uint32_t i = 0; i < RHI::kFramesInFlight; ++i)
            {
                if (!gfx.CreateBuffer(bd, m_rayReadback[i]) ||
                    !(m_rayReadbackMapped[i] = static_cast<const uint32_t*>(gfx.MapBuffer(m_rayReadback[i]))))
                    LOG_ERROR("%s: ray counter readback creation failed", GetName());
            }
        }
#endif
    }

    // The 7 frame-transient scratch textures (rawColor + hitDist + the five NRD
    // textures) - the main view's in the shared alias heap when available,
    // committed fallback inside.
    CreateTransientTextures(gfx, ws, WorkArena());

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.depth      = 1;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    // NRD-resolve output (Lighting.ps reads it through LightCB.rtReflectionIdx).
    // COMMITTED on purpose: on declined / nrdOk == false frames Lighting
    // deliberately folds LAST frame's content - a cross-frame contract no aliased
    // memory can keep.
    // debugName -> ID3D12Resource::SetName. Without it these are "Unnamed" in
    // a RenderDoc capture, and the replay API exposes no compute UAV bindings -
    // so the name is the ONLY way to tell rawColor (pre-denoise) from
    // nrdSpecOut (post-denoise) from denoised (what Lighting folds).
    const std::string denoisedName = std::string(m_dbgPrefix) + "denoised";
    td.debugName = denoisedName.c_str();
    if (!gfx.CreateTexture(td, ws.denoised))
        LOG_ERROR("RTReflectionPass: denoised texture creation failed");
    ws.denoisedState = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName = nullptr;

    RHI::TextureDesc tdR32 = td;
    tdR32.format = RHI::Format::R32_FLOAT;
    // Written only by NRDPack (skipped on the denoiser-bypass instance) and
    // read only by the screen-reuse paths (off there too): not created for
    // that variant; its two root slots get nrdViewZ (R32_FLOAT, arena-
    // aliased, never touched by that instance) as the type-exact stand-in.
    if (!m_bypassDenoiser)
        for (int i = 0; i < 2; ++i)
        {
            if (!gfx.CreateTexture(tdR32, hist.depthHist[i]))
                LOG_ERROR("%s: depth history[%d] creation failed", GetName(), i);
            hist.depthHistState[i] = RHI::ResourceState::SHADER_RESOURCE;
        }

    // FFX hit-counter ping-pong - one R32_UINT texel per 8x8 tile.
    {
        RHI::TextureDesc tdCtr = td;
        tdCtr.width  = ws.tileX;
        tdCtr.height = ws.tileY;
        tdCtr.format = RHI::Format::R32_UINT;
        for (int i = 0; i < 2; ++i)
        {
            if (!gfx.CreateTexture(tdCtr, hist.hitCounter[i]))
                LOG_ERROR("RTReflectionPass: hit counter[%d] creation failed", i);
            hist.hitCounterState[i] = RHI::ResourceState::SHADER_RESOURCE;
        }
    }

    // ---- NRD (REBLUR_SPECULAR) instance + pools ----
    // (The NRD I/O textures are created in CreateTransientTextures; NRD's own
    // permanent / transient pools stay NRD-owned and committed.)
    {
        // NRD is MANDATORY: if it fails, IsUnifiedAvailable() reports false and
        // Renderer falls back to the SSR FullFold chain.
        // The denoiser-bypass instance (and the main view's RR mode) never
        // dispatches NRD: no nrd::Instance, no pools, and no second
        // WaitIdleAndReleaseDeferred on every resize. A view's segment cannot
        // drain the GPU before NRD tears its old pool down: its old instance
        // retires and a fresh one is made.
        if (!m_bypassDenoiser && !RrRawBound())
        {
            if (!mayStall) RetireNrd();
            if (!ws.nrd) ws.nrd = std::make_unique<NrdDenoiser>();
            if (!ws.nrd->Resize(gfx, w, h))
                LOG_WARNING("RTReflectionPass: NRD unavailable — unified hybrid "
                            "reflections disabled (legacy SSR FullFold takes over)");
        }
    }
    // The committed hit-distance copy while ray reconstruction denoises.
    if (m_ws == &m_main) EnsureRRHitDistance(gfx);
}

// The committed copy of the trace's hit distance, render-sized (the main view's, RR mode only).
void RTReflectionPass::EnsureRRHitDistance(IGraphicsDevice& gfx)
{
    if (!m_rrRaw || m_main.w == 0 || m_main.h == 0) return;
    if (m_rrHitDist.IsValid()) return;
    RHI::TextureDesc td{};
    td.width = m_main.w; td.height = m_main.h; td.depth = 1; td.mipLevels = 1;
    td.format = RHI::Format::R16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage = RHI::Usage::DEFAULT;
    td.layout = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName = "RTR.RRHitDistance";
    if (!gfx.CreateTexture(td, m_rrHitDist))
        LOG_ERROR("%s: RR hit distance allocation failed", GetName());
    m_rrHitDistState = td.layout;
    m_rrHitDistWritten = false;
}

void RTReflectionPass::SetRayReconstruction(bool on)
{
    // The front-layer instance never denoises: nothing to switch. The main
    // view's working set (the service point; a secondary view keeps REBLUR).
    if (m_bypassDenoiser || on == m_rrRaw) return;
    m_rrRaw = on;
    ResetHistory();
    if (m_gfx)
    {
        if (on)
        {
            // REBLUR's pools go while RR denoises; the pass is recorded on the graph's
            // workers, so drain before dropping them.
            if (m_main.nrd) { m_gfx->WaitIdleAndReleaseDeferred(); m_main.nrd.reset(); }
            EnsureRRHitDistance(*m_gfx);
        }
        else
        {
            if (m_rrHitDist.IsValid()) m_gfx->DestroyTexture(m_rrHitDist);
            m_rrHitDist = {};
            m_rrHitDistWritten = false;
            if (m_ready && m_main.w && m_main.h && !m_main.nrd)
            {
                m_main.nrd = std::make_unique<NrdDenoiser>();
                if (!m_main.nrd->Resize(*m_gfx, m_main.w, m_main.h))
                    LOG_WARNING("RTReflectionPass: NRD unavailable after ray reconstruction - unified hybrid "
                                "reflections disabled (legacy SSR FullFold takes over)");
            }
        }
    }
    LOG_INFO("[RTR_RR] name=%s rayReconstruction=%u nrd=%u", GetName(), on ? 1u : 0u, m_main.nrd ? 1u : 0u);
}

bool RTReflectionPass::AllResourcesValid() const
{
    const WorkingSet& ws = *m_ws;
    return (!m_swTierPossible || ws.swRayList.IsValid()) && ws.hwRayList.IsValid() &&
           ws.rayCounter.IsValid() && ws.indirectArgs.IsValid() &&
           ws.rawColor.IsValid() && ws.hitDist.IsValid() && ws.denoised.IsValid() &&
           m_hist && (m_bypassDenoiser || (m_hist->depthHist[0].IsValid() && m_hist->depthHist[1].IsValid())) &&
           m_hist->hitCounter[0].IsValid() && m_hist->hitCounter[1].IsValid() &&
           ws.nrdNormalRough.IsValid() && ws.nrdViewZ.IsValid() &&
           ws.nrdMV.IsValid() && ws.nrdSpecIn.IsValid() && ws.nrdSpecOut.IsValid();
}

bool RTReflectionPass::IsUnifiedAvailable() const
{
    if (!m_ready || !AllResourcesValid()) return false;
    if (m_bypassDenoiser) return true;       // NRD is not part of this instance
    if (RrRawBound()) return true;           // RR: ray reconstruction denoises
    return m_useNRD && m_ws->nrd && m_ws->nrd->IsReady();
}

uint64_t RTReflectionPass::GetUpcomingOutputSrv() const
{
    if (!m_ready || !m_gfx) return 0;
    if (!m_ws->denoised.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_ws->denoised);
}

// ==== Execute ====
// clear -> classify -> argsSW -> IntersectSW -> argsHW -> IntersectHW -> pack ->
// NRD -> resolve, all on the graph CL.

RHI::CommandList RTReflectionPass::Execute(RHI::CommandList cl)
{
    WorkingSet& ws = *m_ws;   // the bound view's
    if (!m_ready || !ws.active)
        return cl;
    if (!IsUnifiedAvailable())
        return cl;                      // NRD dead -> Renderer runs FullFold
    const bool rrRaw = RrRawBound();

    // Tier availability: the HW tier needs the TLAS trio; the SW tier needs
    // the SSR toggle + a prev-frame pyramid + a built Hi-Z. Either tier alone
    // keeps the pass alive (SSR-only / RTR-only are valid modes).
    const bool rtOk     = m_intersectHWPSO.IsValid() && ws.fd.tlasVA != 0 && ws.fd.instanceVA != 0 &&
                          ws.fd.bindlessBufferTable != 0;
    const bool externalHiZ = ws.externalHiZSrv != 0 && ws.externalHiZMips != 0;
    const bool ownedHiZ    = ws.hiZ && ws.hiZ->GetSrvHandle() != 0 &&
                             ws.hiZ->GetMipCount() != 0;
    const bool screenOk = m_swTierPossible &&
                          ws.fd.enableScreenTrace != 0 && ws.fd.prevHdrSrv != 0 &&
                          (externalHiZ || ownedHiZ);
    const bool buildOwnedHiZ = screenOk && !externalHiZ;
    const uint64_t screenHiZSrv = externalHiZ ? ws.externalHiZSrv
                                               : (ownedHiZ ? ws.hiZ->GetSrvHandle() : 0);
    const uint32_t screenHiZMips = externalHiZ ? ws.externalHiZMips
                                                : (ownedHiZ ? ws.hiZ->GetMipCount() : 0);
    if (!rtOk && !screenOk)
    {
        // Active frame DECLINED (e.g. SSR just toggled on, pyramid not built
        // yet): the depth-history / hit-counter / REBLUR history all skip
        // this frame - drop them so the resume frame does not validate
        // against N-frame-stale data.
        ResetHistory();
        return cl;
    }

    IGraphicsDevice& gfx = cl.GetDevice();

    // ---- Resolve graph GBuffer textures ----
    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex    = ctx.GetTexture(m_depthH);
    const RHI::Texture* normalTex   = ctx.GetTexture(m_normalH);
    const RHI::Texture* surfaceTex  = ctx.GetTexture(m_surfaceH);
    const RHI::Texture* velocityTex = ctx.GetTexture(m_velocityH);
    if (!depthTex || !normalTex || !surfaceTex || !velocityTex)
    {
        ResetHistory();   // declined active frame - history skips it
        return cl;
    }
    // Grass-free depth snapshot for the Hi-Z build; falls back to main depth.
    const RHI::Texture* traceDepthTex = ctx.GetTexture(m_traceDepthH);

    const uint64_t depthSrv    = gfx.GetTextureSRVGpuHandle(*depthTex);
    const uint64_t normalSrv   = gfx.GetTextureSRVGpuHandle(*normalTex);
    const uint64_t surfaceSrv  = gfx.GetTextureSRVGpuHandle(*surfaceTex);
    const uint64_t velocitySrv = gfx.GetTextureSRVGpuHandle(*velocityTex);
    if (!depthSrv || !normalSrv || !surfaceSrv || !velocitySrv)
    {
        ResetHistory();
        return cl;
    }

    // ---- SW tier: build this frame's Hi-Z pyramid ----
    // Runs FIRST on the graph CL - SSRDepthHierarchyPass binds the SHARED
    // compute root signature; ours is set right after.
    if (buildOwnedHiZ)
    {
        const RHI::Texture* hizSrc = traceDepthTex ? traceDepthTex : depthTex;
        ws.hiZ->Execute(cl, gfx.GetTextureSRVGpuHandle(*hizSrc));
        if (const RHI::Texture* hizTex = ws.hiZ->GetTexture())
            TransitionTex(gfx, *hizTex, ws.hiZTexState,
                          RHI::ResourceState::SHADER_RESOURCE, cl);
    }

    // ---- Ping-pong flip at the TOP (depth history + hit counter) ----
    // (The view's history: IsUnifiedAvailable required it.)
    History& hist = *m_hist;
    hist.writeIdx = 1u - hist.writeIdx;
    const uint32_t writeIdx = hist.writeIdx;
    const uint32_t readIdx  = 1u - hist.writeIdx;

    // ---- Fill CB ----
    {
        using namespace DirectX;
        HybridReflConstants cb{};
        const XMMATRIX vp     = XMLoadFloat4x4(&ws.fd.viewProj);
        const XMMATRIX invVP  = XMMatrixInverse(nullptr, vp);
        const XMMATRIX prevVP = XMLoadFloat4x4(&ws.fd.prevViewProj);
        // HLSL default column-major + mul(vector, matrix) -> upload transposed.
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),  XMMatrixTranspose(invVP));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj), XMMatrixTranspose(prevVP));
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),     XMMatrixTranspose(vp));

        cb.cameraPos[0] = ws.fd.cameraPos.x;
        cb.cameraPos[1] = ws.fd.cameraPos.y;
        cb.cameraPos[2] = ws.fd.cameraPos.z;
        cb.rayTMax      = rayTMax;
        // ONE derivation feeds both consumers so they cannot drift: the pack
        // shader NORMALIZES hit distance with this and REBLUR DENORMALIZES with
        // the copy handed to NrdDenoiser below. Editing only one produces no
        // compile error and no log line - just a globally wrong specular blur
        // radius and virtual-motion reprojection (blurrier, ghosting reflections).
        cb.hitDistParamA = rayTMax * kHitDistAScale;
        cb.materialBufIdx = materialBufBindless;
        cb.exposureBufIdx = exposureBufBindless;
        cb.sceneNprMaxBrightness = sceneNprMaxBrightness;
        cb.sunDirection[0] = ws.fd.sunDirection.x;
        cb.sunDirection[1] = ws.fd.sunDirection.y;
        cb.sunDirection[2] = ws.fd.sunDirection.z;
        cb.roughnessThreshold = roughnessThreshold;
        cb.sunColor[0] = ws.fd.sunColor.x;
        cb.sunColor[1] = ws.fd.sunColor.y;
        cb.sunColor[2] = ws.fd.sunColor.z;
        cb.mirrorRoughness = mirrorRoughness;
        cb.flatAmbient[0] = ws.fd.flatAmbient.x;
        cb.flatAmbient[1] = ws.fd.flatAmbient.y;
        cb.flatAmbient[2] = ws.fd.flatAmbient.z;
        cb.iblStrength = ws.fd.iblStrength;
        cb.screenW = ws.w;
        cb.screenH = ws.h;
        cb.invScreenW = 1.0f / float(ws.w);
        cb.invScreenH = 1.0f / float(ws.h);
        cb.nearZ = ws.fd.nearZ;
        cb.farZ  = ws.fd.farZ;
        cb.normalBias = normalBias;
        cb.ssrConfidenceThreshold = ssrConfidenceThreshold;
        cb.depthThicknessBase  = depthThicknessBase;
        cb.thicknessRoughScale = thicknessRoughScale;
        cb.thicknessLenFactor  = thicknessLenFactor;
        cb.maxTraversalIters   = maxTraversalIters;
        cb.hybridSpawnRate  = hybridSpawnRate;
        cb.hybridMissWeight = hybridMissWeight;
        cb.tileCountX = ws.tileX;
        cb.tileCountY = ws.tileY;
        cb.frameIndex   = ws.fd.frameIndex;
        cb.resetHistory = hist.reset ? 1u : 0u;
        // Tier gates: SW marches only when hizMipCount > 0 AND bit0; HW rays only
        // behind bit1 (TLAS trio validated above). bit2: the resolve passes rawColor
        // through (bypass / RR). bit3: local lights at HW hits unshadowed
        // (-rtr_unshadowed_hit_lights, an A/B arm).
        static const bool s_unshadowedHitLights = Sys::CommandLine::Get().Has("-rtr_unshadowed_hit_lights");
        cb.unifiedFlags = (screenOk ? 1u : 0u) | (rtOk ? 2u : 0u)
                        | ((m_bypassDenoiser || rrRaw) ? 4u : 0u)
                        | (s_unshadowedHitLights ? 8u : 0u);
        cb.rayMask      = m_rayMask;
        cb.worldLightGridIndex = ws.fd.worldLightGridIndex;
        // Last frame's ReSTIR GI for the environment stand-in.
        {
            const uint32_t giIdx = (m_giResultH.IsValid() && ws.fd.restirGiPrevSrv)
                ? gfx.DescriptorHeapIndexFromGpuHandle(ws.fd.restirGiPrevSrv) : 0u;
            cb.restirGiPrevIdx = giIdx == ~0u ? 0u : giIdx;
        }
        cb.hizMipCount  = screenOk ? screenHiZMips : 0u;
        cb.ddgiVolumeCount = ws.fd.ddgiVolumeCount;
        cb.lightCount      = ws.fd.lightCount;
        cb.ddgiDiffuseScale   = ws.fd.ddgiDiffuseScale;
        cb.hasSkySH   = (ws.fd.skySHSrv != 0 && ws.fd.iblUseSH != 0) ? 1u : 0u;
        cb.hasPrevHdr = (ws.fd.prevHdrSrv != 0) ? 1u : 0u;
        cb.prevHdrMaxMip = (ws.fd.prevHdrMips > 0)
                         ? float(ws.fd.prevHdrMips - 1) : 0.0f;
        cb.hasSkyCube  = (ws.fd.skyCubeSrv != 0 && ws.fd.skyCubeIsReal) ? 1u : 0u;
        cb.skyCubeMips = (ws.fd.skyCubeMips > 0) ? ws.fd.skyCubeMips : 1u;
        cb.ddgiAONearFieldStrength = ws.fd.ddgiAONearFieldStrength;
        // The probe buffer is opened from the heap (probeBufferIdx). probeArraySrv
        // stays in the gate: the cube array is not bound, but it is the "probes are
        // resident" signal.
        {
            // A typed handle, the stride checked; with no resident probes a null index
            // and no probes to walk.
            const bool probes = ws.fd.probeArraySrv && ws.fd.probeBufferSrv;
            cb.probeCount     = probes ? ws.fd.probeCount : 0u;
            cb.probeBufferIdx = gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
                probes ? ws.fd.probeBufferSrv : 0ull, "HybridRefl probe buffer",
                static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();
        }
        cb.missEnvFallback = missEnvFallback ? 1u : 0u;
        // Alpha-cutout gate. All three resources must be live or the shader
        // must not touch g_AlphaRecords at all (its root SRV may be VA 0).
        // This is the "table not live this frame" guard, NOT the kill switch -
        // that is p.alphaClip in DDGISceneAS's fillMaterialProxy, since clearing
        // this flag leaves instances FORCE_NON_OPAQUE and traversal still exits
        // fixed-function on every candidate.
        cb.alphaTestEnabled = (ws.fd.alphaRecordVA && ws.fd.bindlessBufferTable
                               && m_bindlessTexTable) ? 1u : 0u;
        cb.alphaInstanceCount = ws.fd.alphaInstanceCount;
        if (auto* slot = m_cb.Current(gfx)) *slot = cb;
    }

    // ---- Alias-acquire the placed transients (shared arena residents) ----
    // Recorded EVERY frame this pass runs, before any transition/write: the
    // heap bytes may have last belonged to XeGTAO/VolFog/Bloom (or nobody -
    // null `before` = "any resource"). Content is garbage afterwards; the
    // full-coverage Classify/NRDPack/REBLUR writes below re-initialize it.
    // State mirrors are deliberately NOT reset - both the engine tracker and
    // the debug layer track per-resource state THROUGH aliasing barriers.
    if (ws.placedMask)
    {
        const RHI::GPUResource* placed[] = {
            &ws.rawColor, &ws.hitDist, &ws.nrdNormalRough, &ws.nrdViewZ,
            &ws.nrdMV, &ws.nrdSpecIn, &ws.nrdSpecOut,
        };
        for (int i = 0; i < int(sizeof(placed) / sizeof(placed[0])); ++i)
            if (ws.placedMask & (1u << i))
                gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, placed[i]), cl);
    }

    // ---- Transition pass-owned textures for this frame's writes ----
    TransitionTex(gfx, ws.rawColor, ws.rawColorState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.hitDist,  ws.hitDistState,  RHI::ResourceState::UNORDERED_ACCESS, cl);
    const bool hasDepthHist = hist.depthHist[0].IsValid() && hist.depthHist[1].IsValid();
    if (hasDepthHist)
        TransitionTex(gfx, hist.depthHist[writeIdx],  hist.depthHistState[writeIdx],
                      RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, hist.hitCounter[writeIdx], hist.hitCounterState[writeIdx],
                  RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, hist.hitCounter[readIdx],  hist.hitCounterState[readIdx],
                  RHI::ResourceState::SHADER_RESOURCE, cl);

    // ---- Our layout + all stage-invariant bindings ----
    // Binding the first stage's PSO sets the layout. Descriptor tables persist
    // across the stages' PSO binds - one layout, and D3D12 invalidates root
    // arguments only when the root signature changes; resources are only
    // STATE-validated for views a dispatch actually accesses, so binding the
    // whole map once up front is safe. Every table must still hold a VALID
    // descriptor of the right dimension even when the feature is off (the
    // shaders gate reads on CB flags).
    gfx.BindComputePipelineState(m_clearCountersPSO, cl);
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(kTLAS, ws.fd.tlasVA, cl);
    gfx.SetComputeRootSRVAddress(kInstances, ws.fd.instanceVA, cl);
    // Bound unconditionally, exactly like kInstances: VA 0 is what an inert
    // frame already passes, and a root descriptor the shader DECLARES but that
    // is never set is a debug-layer error at Dispatch. Safety comes from the
    // shader's own flag/range checks, not from skipping the bind.
    gfx.SetComputeRootSRVAddress(kAlphaRecords, ws.fd.alphaRecordVA, cl);

    const uint64_t rawColorSrv = gfx.GetTextureSRVGpuHandle(ws.rawColor);
    const uint64_t ctrBufSrv   = gfx.GetBufferSRVGpuHandle(ws.rayCounter);

    gfx.SetComputeDescriptorTable(kSRV_Depth,    depthSrv,    cl);
    gfx.SetComputeDescriptorTable(kSRV_Normal,   normalSrv,   cl);
    gfx.SetComputeDescriptorTable(kSRV_Surface,  surfaceSrv,  cl);
    gfx.SetComputeDescriptorTable(kSRV_Velocity, velocitySrv, cl);
    gfx.SetComputeDescriptorTable(kSRV_Lights,
        ws.fd.lightsSrv ? ws.fd.lightsSrv : ctrBufSrv, cl);
    gfx.SetComputeDescriptorTable(kSRV_DDGIVolumes,
        ws.fd.ddgiVolumeBufSrv ? ws.fd.ddgiVolumeBufSrv : ctrBufSrv, cl);
    if (ws.fd.ddgiIrradianceTable)   gfx.SetComputeDescriptorTable(kSRV_DDGISH,    ws.fd.ddgiIrradianceTable,   cl);
    if (ws.fd.ddgiDepthTable)     gfx.SetComputeDescriptorTable(kSRV_DDGIDepth, ws.fd.ddgiDepthTable,     cl);
    if (ws.fd.ddgiProbeDataTable) gfx.SetComputeDescriptorTable(kSRV_DDGIProbe, ws.fd.ddgiProbeDataTable, cl);

    // Prev-frame pyramid + depth history (SW radiance reuse). PrevHDR falls
    // back to a valid float4 texture; shader gates on hasPrevHdr.
    gfx.SetComputeDescriptorTable(kSRV_PrevHDR,
        ws.fd.prevHdrSrv ? ws.fd.prevHdrSrv : rawColorSrv, cl);
    // Absent depth history (bypass instance) -> ws.nrdViewZ, the same R32_FLOAT
    // shape (Texture2D<float> / RWTexture2D<float>), bound but never accessed.
    gfx.SetComputeDescriptorTable(kSRV_PrevDepth,
        gfx.GetTextureSRVGpuHandle(hasDepthHist ? hist.depthHist[readIdx] : ws.nrdViewZ), cl);

    gfx.SetComputeDescriptorTable(kSRV_SkySH,
        ws.fd.skySHSrv ? ws.fd.skySHSrv : ctrBufSrv, cl);
    if (ws.fd.skyCubeSrv)
        gfx.SetComputeDescriptorTable(kSRV_SkyCube, ws.fd.skyCubeSrv, cl);
    if (const uint64_t irr = ws.fd.skyIrrSrv ? ws.fd.skyIrrSrv : ws.fd.skyCubeSrv)
        gfx.SetComputeDescriptorTable(kSRV_SkyIrr, irr, cl);

    // Hi-Z (t27). Fallback: velocity - a valid float2 descriptor; the shader
    // reads gDepthHier only behind hizMipCount > 0.
    gfx.SetComputeDescriptorTable(kSRV_DepthHier,
        screenOk ? screenHiZSrv : velocitySrv, cl);

    gfx.SetComputeDescriptorTable(kSRV_RawColor, rawColorSrv, cl);
    gfx.SetComputeDescriptorTable(kSRV_HitDist,  gfx.GetTextureSRVGpuHandle(ws.hitDist), cl);
    gfx.SetComputeDescriptorTable(kSRV_NrdSpecOut, gfx.GetTextureSRVGpuHandle(ws.nrdSpecOut), cl);
    gfx.SetComputeDescriptorTable(kSRV_HitCounterHist,
        gfx.GetTextureSRVGpuHandle(hist.hitCounter[readIdx]), cl);

    gfx.SetComputeDescriptorTable(kUAV_RawColor,     gfx.GetTextureUAVGpuHandle(ws.rawColor), cl);
    gfx.SetComputeDescriptorTable(kUAV_HitDist,      gfx.GetTextureUAVGpuHandle(ws.hitDist), cl);
    // Absent SW ray list (no SW tier on this instance) -> the HW list's UAV:
    // identical structured desc, never written through this slot there.
    gfx.SetComputeDescriptorTable(kUAV_SWRayList,
        gfx.GetBufferUAVGpuHandle(ws.swRayList.IsValid() ? ws.swRayList : ws.hwRayList), cl);
    gfx.SetComputeDescriptorTable(kUAV_HWRayList,    gfx.GetBufferUAVGpuHandle(ws.hwRayList), cl);
    gfx.SetComputeDescriptorTable(kUAV_RayCounter,   gfx.GetBufferUAVGpuHandle(ws.rayCounter), cl);
    gfx.SetComputeDescriptorTable(kUAV_IndirectArgs, gfx.GetBufferUAVGpuHandle(ws.indirectArgs), cl);
    gfx.SetComputeDescriptorTable(kUAV_HitCounter,
        gfx.GetTextureUAVGpuHandle(hist.hitCounter[writeIdx]), cl);
    gfx.SetComputeDescriptorTable(kUAV_Denoised,     gfx.GetTextureUAVGpuHandle(ws.denoised), cl);
    gfx.SetComputeDescriptorTable(kUAV_DepthHistory,
        gfx.GetTextureUAVGpuHandle(hasDepthHist ? hist.depthHist[writeIdx] : ws.nrdViewZ), cl);
    gfx.SetComputeDescriptorTable(kUAV_NrdNormalRough, gfx.GetTextureUAVGpuHandle(ws.nrdNormalRough), cl);
    gfx.SetComputeDescriptorTable(kUAV_NrdViewZ,       gfx.GetTextureUAVGpuHandle(ws.nrdViewZ), cl);
    gfx.SetComputeDescriptorTable(kUAV_NrdMV,          gfx.GetTextureUAVGpuHandle(ws.nrdMV), cl);
    gfx.SetComputeDescriptorTable(kUAV_NrdSpecIn,      gfx.GetTextureUAVGpuHandle(ws.nrdSpecIn), cl);

    // Bindless VB/IB only exist when the HW tier is live.
    if (ws.fd.bindlessBufferTable)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, ws.fd.bindlessBufferTable, cl);
    if (m_bindlessTexTable)
        gfx.SetComputeDescriptorTable(kBindlessTex, m_bindlessTexTable, cl);

    auto uavBarrier = [&]() { gfx.PushBarrier(RHI::GPUBarrier::Memory(), cl); };
    if (!ws.indirectArgs.IsValid())
    {
        hist.reset = true;
        return cl;
    }

    // Per-stage GPU timestamp regions: the pass is the most expensive thing on
    // the GPU (~30% of a frame), and the ray trace, the full-res NRD pack and the
    // REBLUR denoise need completely different fixes. Nesting inside the graph's
    // own per-pass region is fine: GPUProfiler computes an interval UNION, so a
    // child region does not double-count into its parent. Safe on a worker
    // thread too - region indices come from an atomic and the Tracy GPU zone
    // opens and closes on this same thread.
    struct StageTimer
    {
        IGraphicsDevice& g; RHI::CommandList c; uint32_t r;
        StageTimer(IGraphicsDevice& gg, RHI::CommandList cc, const char* n)
            : g(gg), c(cc), r(gg.BeginGPUTimestamp(cc, n)) {}
        ~StageTimer() { g.EndGPUTimestamp(c, r); }
    };

    // ---- 1. Clear ray counters (removes any cross-frame dependency) ----
    {
        StageTimer t(gfx, cl, m_stageNames[0]);
        gfx.BindComputePipelineState(m_clearCountersPSO, cl);
        gfx.DispatchCompute(1, 1, 1, cl);
        uavBarrier();
    }

    // ---- 2. Classify: ray routing + lists + hit-counter seed + zero-fill ----
    {
        StageTimer t(gfx, cl, m_stageNames[1]);
        gfx.BindComputePipelineState(m_classifyPSO, cl);
        gfx.DispatchCompute(ws.tileX, ws.tileY, 1, cl);
        uavBarrier();
    }

    // ---- 3. SW tier: args -> indirect Hi-Z intersect ----
    if (screenOk)
    {
        StageTimer t(gfx, cl, m_stageNames[2]);
        gfx.BindComputePipelineState(m_prepareArgsSWPSO, cl);
        gfx.DispatchCompute(1, 1, 1, cl);
        uavBarrier();

        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.indirectArgs,
            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::INDIRECT_ARGUMENT), cl);
        gfx.BindComputePipelineState(m_intersectSWPSO, cl);
        gfx.DispatchComputeIndirect(ws.indirectArgs, kArgsSWByteOffset, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.indirectArgs,
            RHI::ResourceState::INDIRECT_ARGUMENT, RHI::ResourceState::UNORDERED_ACCESS), cl);
        uavBarrier();
    }

    // ---- 4. HW tier: args (classify + SW demotions) -> indirect RayQuery ----
    if (rtOk)
    {
        // The RayQuery tier's own region: contention with other RT work (RTShadow /
        // RTAO in the DDGI overlap window) would inflate this region, not only the
        // pass total.
        StageTimer t(gfx, cl, m_stageNames[3]);
        gfx.BindComputePipelineState(m_prepareArgsHWPSO, cl);
        gfx.DispatchCompute(1, 1, 1, cl);
        uavBarrier();

        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.indirectArgs,
            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::INDIRECT_ARGUMENT), cl);
        gfx.BindComputePipelineState(m_intersectHWPSO, cl);
        gfx.DispatchComputeIndirect(ws.indirectArgs, kArgsHWByteOffset, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.indirectArgs,
            RHI::ResourceState::INDIRECT_ARGUMENT, RHI::ResourceState::UNORDERED_ACCESS), cl);
        uavBarrier();
    }

    // ---- 5. Pack: GBuffer + intersect outputs -> the NRD input quartet ----
    // A full-res Dispatch((w+7)/8,(h+7)/8): a cost independent of the ray count.
    //
    // Denoiser BYPASS (TranslucentFront): steps 5-7 are skipped entirely -
    // the raw trace is parked SRV-readable and NRDResolve (unifiedFlags bit2)
    // passes rawColor straight through, which keeps the second instance's fixed
    // full-res cost to Classify + Resolve.
    bool nrdOk = false;
    if (m_bypassDenoiser)
    {
        TransitionTex(gfx, ws.rawColor, ws.rawColorState, RHI::ResourceState::SHADER_RESOURCE, cl);
        TransitionTex(gfx, ws.hitDist,  ws.hitDistState,  RHI::ResourceState::SHADER_RESOURCE, cl);
        nrdOk = true;
    }
    else
    {
    {
        StageTimer t(gfx, cl, m_stageNames[4]);
        TransitionTex(gfx, ws.rawColor, ws.rawColorState, RHI::ResourceState::SHADER_RESOURCE, cl);
        TransitionTex(gfx, ws.hitDist,  ws.hitDistState,  RHI::ResourceState::SHADER_RESOURCE, cl);
        TransitionTex(gfx, ws.nrdNormalRough, ws.nrdNormalRoughState, RHI::ResourceState::UNORDERED_ACCESS, cl);
        TransitionTex(gfx, ws.nrdViewZ,       ws.nrdViewZState,       RHI::ResourceState::UNORDERED_ACCESS, cl);
        TransitionTex(gfx, ws.nrdMV,          ws.nrdMVState,          RHI::ResourceState::UNORDERED_ACCESS, cl);
        TransitionTex(gfx, ws.nrdSpecIn,      ws.nrdSpecInState,      RHI::ResourceState::UNORDERED_ACCESS, cl);
        gfx.BindComputePipelineState(m_nrdPackPSO, cl);
        gfx.DispatchCompute((ws.w + 7) / 8, (ws.h + 7) / 8, 1, cl);
        uavBarrier();
    }

    // RR: ray reconstruction denoises; the pack above kept the screen
    // tier's depth history, and the resolve passes rawColor through.
    if (rrRaw) nrdOk = true;
    else
    {
    // ---- 6. Hand the quartet to NRD (inputs NPSR, output UAV) ----
    // NRD restores these exact states before returning.
    TransitionTex(gfx, ws.nrdNormalRough, ws.nrdNormalRoughState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.nrdViewZ,       ws.nrdViewZState,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.nrdMV,          ws.nrdMVState,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.nrdSpecIn,      ws.nrdSpecInState,      RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.nrdSpecOut,     ws.nrdSpecOutState,     RHI::ResourceState::UNORDERED_ACCESS, cl);

    NrdDenoiser::Params np{};
    std::memcpy(np.viewToClip,  &ws.fd.projMatrixNoJitter, sizeof(np.viewToClip));
    std::memcpy(np.worldToView, &ws.fd.viewMatrix,         sizeof(np.worldToView));
    np.jitter[0]      = ws.fd.jitterX;
    np.jitter[1]      = ws.fd.jitterY;
    np.frameIndex     = ws.fd.frameIndex;
    np.resetHistory   = hist.reset;
    np.denoisingRange = ws.fd.farZ * 0.99f;
    np.width          = ws.w;
    np.height         = ws.h;
    // MUST equal cb.hitDistParamA above - same expression on purpose.
    np.specHitDistScaleA = rayTMax * kHitDistAScale;

    auto bind = [&](nrd::ResourceType type, const RHI::Texture& t, RHI::ResourceState st) {
        NrdDenoiser::Binding b;
        b.type        = type;
        b.tex.texture = &t;
        b.tex.state   = st;
        return b;
    };
    const NrdDenoiser::Binding nrdBindings[] = {
        bind(nrd::ResourceType::IN_MV,                     ws.nrdMV,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS,       ws.nrdNormalRough, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_VIEWZ,                  ws.nrdViewZ,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,  ws.nrdSpecIn,      RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, ws.nrdSpecOut,     RHI::ResourceState::UNORDERED_ACCESS),
    };
    // REBLUR_SPECULAR. A fixed full-res cost that does NOT shrink when ray
    // count drops - if this dominates, cutting rays is the wrong lever.
    {
        StageTimer t(gfx, cl, m_stageNames[5]);
        nrdOk = ws.nrd->Denoise(gfx, cl, np, nrdBindings, 5);
    }

    // ---- 7. NRD replaced the heaps + root signature - restore ours ----
    gfx.BindDescriptorHeaps(cl);
    gfx.BindComputePipelineState(m_nrdResolvePSO, cl);   // our layout back, every root argument unset
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeDescriptorTable(kSRV_Depth,   depthSrv,   cl);
    gfx.SetComputeDescriptorTable(kSRV_Surface, surfaceSrv, cl);
    gfx.SetComputeDescriptorTable(kSRV_RawColor, rawColorSrv, cl);
    }   // !rrRaw
    }   // !m_bypassDenoiser

    // ---- 8. Resolve NRD output (+ conf) -> ws.denoised ----
    {
        StageTimer t(gfx, cl, m_stageNames[6]);
        TransitionTex(gfx, ws.nrdSpecOut, ws.nrdSpecOutState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
        // With the output imported the graph transitions it (to UAV in this pass's
        // Phase A, back for each reader); this and step 9's are the pass's own only
        // without the import (-rgimportoff).
        if (!m_denoisedH.IsValid())
            TransitionTex(gfx, ws.denoised, ws.denoisedState, RHI::ResourceState::UNORDERED_ACCESS, cl);
        gfx.SetComputeDescriptorTable(kSRV_NrdSpecOut, gfx.GetTextureSRVGpuHandle(ws.nrdSpecOut), cl);
        gfx.SetComputeDescriptorTable(kUAV_Denoised,   gfx.GetTextureUAVGpuHandle(ws.denoised), cl);
        if (nrdOk)
        {
            gfx.BindComputePipelineState(m_nrdResolvePSO, cl);
            gfx.DispatchCompute((ws.w + 7) / 8, (ws.h + 7) / 8, 1, cl);
        }
    }
    // nrdOk == false: ws.denoised keeps last frame's content - LightingPass
    // still reads a defined texture; Denoise() logged the warning.

    // Count the actual indirect dispatch lists. These buffers are populated by
    // PrepareArgs, so this measures executed primary rays, not a WxH estimate.
    // Copy after the last shader reader, and restore UAV for next frame's clear.
    // The main view's only.
    const uint32_t readbackSlot = gfx.GetFrameIndex();
    if (m_ws == &m_main && m_rayReadbackMapped[readbackSlot])
    {
        if (m_rayReadbackPrimed[readbackSlot])
        {
            const auto* counters = m_rayReadbackMapped[readbackSlot];
            LOG_INFO("[RTRAYS] name=%s frame=%llu sw=%u hw=%u pixels=%u primaryOnly=1",
                GetName(), static_cast<unsigned long long>(m_rayReadbackFrame[readbackSlot]),
                counters[1], counters[3], ws.w * ws.h);
        }
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.rayCounter,
            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::COPY_SRC), cl);
        gfx.CopyBuffer(ws.rayCounter, m_rayReadback[readbackSlot], 8 * sizeof(uint32_t), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&ws.rayCounter,
            RHI::ResourceState::COPY_SRC, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_rayReadbackPrimed[readbackSlot] = true;
        m_rayReadbackFrame[readbackSlot] = m_rayReadbackSequence++;
    }

    // ---- 8b. RR: keep the hit distance past the transient arena ----
    // ws.hitDist is aliased with later passes' scratch; the post chain's
    // RRGuidePass reads this committed copy (same format and size). The main
    // view's only (rrRaw).
    if (m_ws == &m_main) m_rrHitDistWritten = false;
    if (rrRaw && m_rrHitDist.IsValid() && ws.hitDist.IsValid())
    {
        TransitionTex(gfx, ws.hitDist,   ws.hitDistState,   RHI::ResourceState::COPY_SRC, cl);
        TransitionTex(gfx, m_rrHitDist, m_rrHitDistState, RHI::ResourceState::COPY_DST, cl);
        gfx.CopyTexture(ws.hitDist, m_rrHitDist, cl);
        TransitionTex(gfx, m_rrHitDist, m_rrHitDistState, RHI::ResourceState::SHADER_RESOURCE, cl);
        TransitionTex(gfx, ws.hitDist,   ws.hitDistState,   RHI::ResourceState::SHADER_RESOURCE, cl);
        m_rrHitDistWritten = true;
    }

    // ---- 9. Park outputs for their next readers ----
    if (!m_denoisedH.IsValid())
        TransitionTex(gfx, ws.denoised, ws.denoisedState, RHI::ResourceState::SHADER_RESOURCE, cl);
    if (hasDepthHist)
        TransitionTex(gfx, hist.depthHist[writeIdx],  hist.depthHistState[writeIdx],
                      RHI::ResourceState::SHADER_RESOURCE, cl);
    TransitionTex(gfx, hist.hitCounter[writeIdx], hist.hitCounterState[writeIdx],
                  RHI::ResourceState::SHADER_RESOURCE, cl);

    // Hi-Z pyramid back to UAV for next frame's build dispatches.
    if (buildOwnedHiZ)
        if (const RHI::Texture* hizTex = ws.hiZ->GetTexture())
            TransitionTex(gfx, *hizTex, ws.hiZTexState,
                          RHI::ResourceState::UNORDERED_ACCESS, cl);

    hist.reset = false;
    return cl;
}
