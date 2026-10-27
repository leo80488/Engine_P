#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/MeshDescriptorHeap.h"   // kMaxBuffers (bindless VB/IB table)
#include "Graphics/NrdDenoiser.h"          // SIGMA_SHADOW denoiser
#include "RenderGraph/PassTextureNames.h"
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
std::vector<uint8_t> CompileShaderFile(const std::string& path, const std::string& entry)
{
    return ShaderDiskCache::LoadOrCompile(path, entry, RHI::ShaderStage::CS, {}, "RTShadowPass");
}

// Root parameter indices - MUST stay in sync with RTShadow.cs.hlsl +
// RTShadowResolve.cs.hlsl. The SIGMA input/output textures cross into NRD via
// NrdDenoiser::Denoise (its own root sig), NOT this one - this root sig only
// covers the trace+pack dispatch and the resolve dispatch.
constexpr RHI::Slots::CmpParam kCB              { 0 };   // b0
constexpr RHI::Slots::CmpParam kTLAS           { 1 };   // t0 (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kSRV_Depth      { 2 };   // t1
constexpr RHI::Slots::CmpParam kSRV_Normal     { 3 };   // t2
constexpr RHI::Slots::CmpParam kSRV_Surface    { 4 };   // t3
constexpr RHI::Slots::CmpParam kSRV_Velocity   { 5 };   // t4
constexpr RHI::Slots::CmpParam kSRV_ShadowIn   { 6 };   // t5 (resolve: SIGMA output)
constexpr RHI::Slots::CmpParam kSRV_DepthR     { 7 };   // t6 (resolve: sky force-lit)
constexpr RHI::Slots::CmpParam kUAV_Penumbra   { 8 };   // u0
constexpr RHI::Slots::CmpParam kUAV_NormalRough{ 9 };   // u1
constexpr RHI::Slots::CmpParam kUAV_ViewZ      { 10 };  // u2
constexpr RHI::Slots::CmpParam kUAV_MV         { 11 };  // u3
constexpr RHI::Slots::CmpParam kUAV_Final      { 12 };  // u4
// Alpha-cutout occluders: the record table plus the two bindless heaps the
// candidate test walks (VB/IB for UVs, textures for the cutout alpha).
constexpr RHI::Slots::CmpParam kAlphaRecords   { 13 };  // t7 space0 (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kBindlessBuffers{ 14 };  // t0.. space1
constexpr RHI::Slots::CmpParam kBindlessTex    { 15 };  // t0.. space2
// SIGMA_SHADOW_TRANSLUCENCY IN_TRANSLUCENCY: .x = lit bit, .yzw = transmittance
// accumulated across alpha-BLEND occluders. Trace-only.
constexpr RHI::Slots::CmpParam kUAV_Translucency { 16 }; // u5
// Per-instance data (RTInstanceData, 128 B) - bound ONLY so the candidate
// cutout test can apply the material UV transform, which the compact 40 B
// alpha record deliberately does not carry. Same InstanceID() index space as
// kAlphaRecords, and gated by the same cb.alphaTestEnabled.
constexpr RHI::Slots::CmpParam kInstances      { 17 };  // t8 space0 (root SRV, raw VA)
constexpr uint32_t kNumRootParams  = 18;

void TransitionTex(IGraphicsDevice& gfx, const RHI::Texture& tex,
                   RHI::ResourceState& state, RHI::ResourceState to, RHI::CommandList cl)
{
    if (state == to || !tex.IsValid()) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}
} // namespace

RTShadowPass::RTShadowPass(RG::RGTextureHandle normal,
                           RG::RGTextureHandle surface,
                           RG::RGTextureHandle depth,
                           RG::RGTextureHandle velocity)
    : m_normalH(normal), m_surfaceH(surface), m_depthH(depth), m_velocityH(velocity)
{
}

// The resolved mask (RGBA8, the render size: the colour of the sun's light, .a its largest
// channel) as this pass's import - the graph
// reads the member at each Execute and transitions it (Graph tracking):
// UNORDERED_ACCESS in this pass's Phase A, SHADER_RESOURCE in each reader's. The
// member is the bound view's mask (SyncImports).
void RTShadowPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_finalH = d.Import(RG::PassTex::kRTShadowMask, { RHI::Format::R8G8B8A8_UNORM, 0, 0, false, true },
                        { &m_finalImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
}

void RTShadowPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_normalH);
    b.ReadSRV(m_surfaceH);
    b.ReadSRV(m_depthH);
    b.ReadSRV(m_velocityH);
    b.WriteUAV(m_finalH);   // the graph brings the mask to UNORDERED_ACCESS
}

void RTShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    if (!gfx.GetCapabilities().inlineRaytracing)
    {
        LOG_INFO("RTShadowPass: device does not support DXR — pass disabled");
        m_ready = false;
        return;
    }
    if (!BuildRootSignature(gfx)) return;
    // The pipelines: BuildPipelines, the first time RT shadows are wanted
    // (EnsureResources) - see RTAOPass::Init.
    m_bindlessTexTable = gfx.GetBindlessTextureTableGpuHandle();
    if (!m_cb.Create(gfx, "RTShadow.CB"))
    {
        LOG_ERROR("RTShadowPass: CB creation failed");
        return;
    }
    // m_ready is finalised in OnCompile once the NRD SIGMA instance is up.
    m_ready = true;
    LOG_INFO("RTShadowPass: initialised (inline RayQuery + NRD "
             "SIGMA_SHADOW_TRANSLUCENCY)");
}

void RTShadowPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_layout.IsValid()) return;   // no DXR, or Init failed
    gfx.DestroyPipelineState(m_tracePSO);
    gfx.DestroyPipelineState(m_resolvePSO);
    m_pipelinesFailed = false;
    // Rebuild the root signature too: rebuilding only the PSOs would feed a
    // root-sig edit's NEW shader to the OLD signature, fail
    // CreateComputePipelineState and disable the pass until the next reload.
    gfx.DestroyPipelineLayout(m_layout);
    if (!BuildRootSignature(gfx))
    {
        LOG_ERROR("RTShadowPass: root signature rebuild failed — pass disabled");
        m_ready = false;
        return;
    }
    // In use: rebuilt now; otherwise on the next EnsureResources(true), from
    // the new source (see RTAOPass::ReloadShaders).
    if (m_resourcesWanted && !BuildPipelines("shader reload")) return;
    RequestHistoryReset();
}

bool RTShadowPass::BuildPipelines(const char* why)
{
    if (m_tracePSO.IsValid() && m_resolvePSO.IsValid()) return true;
    if (m_pipelinesFailed || !m_layout.IsValid() || !m_gfx) return false;
    const auto start = std::chrono::steady_clock::now();
    if (!CreatePipelines(*m_gfx))
    {
        m_gfx->DestroyPipelineState(m_tracePSO);
        m_gfx->DestroyPipelineState(m_resolvePSO);
        m_pipelinesFailed = true;
        m_ready = false;
        LOG_ERROR("RTShadowPass: pipelines failed — pass disabled until a shader reload");
        return false;
    }
    LOG_INFO("RTShadowPass: pipelines built on %s (%.0f ms)", why,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

bool RTShadowPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    RHI::PipelineLayoutBuilder b("RTShadow");
    b.RootCBV(kCB, 0)
     .RootSRV(kTLAS, 0)            // t0 - TLAS
     // Per-instance alpha-cutout records (RTAlphaRecord, 40 B), same
     // InstanceID() index space as the RTR instance buffer.
     .RootSRV(kAlphaRecords, 7)    // t7
     // Instance table - supplies the material UV transform to the cutout test.
     .RootSRV(kInstances, 8)       // t8
     .Table(kSRV_Depth,        { RHI::SrvRange(1) })
     .Table(kSRV_Normal,       { RHI::SrvRange(2) })
     .Table(kSRV_Surface,      { RHI::SrvRange(3) })
     .Table(kSRV_Velocity,     { RHI::SrvRange(4) })
     .Table(kSRV_ShadowIn,     { RHI::SrvRange(5) })
     .Table(kSRV_DepthR,       { RHI::SrvRange(6) })
     .Table(kUAV_Penumbra,     { RHI::UavRange(0) })
     .Table(kUAV_NormalRough,  { RHI::UavRange(1) })
     .Table(kUAV_ViewZ,        { RHI::UavRange(2) })
     .Table(kUAV_MV,           { RHI::UavRange(3) })
     .Table(kUAV_Final,        { RHI::UavRange(4) })
     .Table(kUAV_Translucency, { RHI::UavRange(5) })
     // The two bindless tables the alpha-cutout candidate test walks.
     .Table(kBindlessBuffers,  { RHI::SrvRange(0, MeshDescriptorHeap::kMaxBuffers, 1) })
     .Table(kBindlessTex,      { RHI::SrvRange(0, GraphicsDX12::kMaxBindlessTextures, 2) });
    // s1 - linear WRAP, matching the material sampler the raster alpha clip
    // uses. MaxLOD stays FLT_MAX (SamplerDesc's default): zero clamps every
    // SampleLevel to mip 0 and silently kills the alphaMipLevel knob.
    RHI::SamplerDesc linearWrap{};
    linearWrap.maxAnisotropy = 1;
    b.Sampler(linearWrap, 1);
    // HEAP_DIRECTLY_INDEXED like the shared graphics/compute signatures, DDGI,
    // RTR and the RTDI shell: whether a shader may open ResourceDescriptorHeap[]
    // does not depend on which pass binds it.
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("RTShadowPass: pipeline layout failed");
        return false;
    }
    return true;
}

bool RTShadowPass::CreatePipelines(IGraphicsDevice& gfx)
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
        if (!ok) LOG_ERROR("RTShadowPass: pipeline failed for %s", path.c_str());
        return ok;
    };
    if (!buildCS("shaders/RTShadow.cs.hlsl",        m_tracePSO))   return false;
    if (!buildCS("shaders/RTShadowResolve.cs.hlsl", m_resolvePSO)) return false;
    return true;
}

void RTShadowPass::DestroyResolutionResources(IGraphicsDevice& gfx)
{
    WorkingSet& ws = *m_ws;
    for (RHI::Texture* tex : { &ws.penumbra, &ws.normalRough, &ws.viewZ, &ws.mv,
                               &ws.translucency, &ws.shadowOut, &ws.final })
        if (tex->IsValid()) gfx.DestroyTexture(*tex);
    ws.penumbra = {}; ws.normalRough = {}; ws.viewZ = {}; ws.mv = {};
    ws.translucency = {}; ws.shadowOut = {}; ws.final = {};
    ws.ready = false;
}

void RTShadowPass::RetireNrd()
{
    if (m_ws->nrd) m_retiredNrd.emplace_back(std::move(m_ws->nrd), RHI::kFramesInFlight + 1u);
}

void RTShadowPass::EnsureResources(bool wanted)
{
    // Same contract as RTAOPass::EnsureResources - see that definition for
    // the full rationale (intent-driven, main-thread, drain-then-release). The
    // frame boundary: the main view is bound.
    m_resourcesWanted = wanted;
    if (!m_gfx || !m_layout.IsValid()) return;   // no DXR, or Init failed

    if (wanted)
    {
        if (BuildPipelines("first use")) OnCompile(*m_gfx);   // pipelines first, built once
        return;
    }
    if (!m_ws->final.IsValid() && !m_ws->nrd) return;   // already released

    m_gfx->WaitIdleAndReleaseDeferred();
    DestroyResolutionResources(*m_gfx);
    m_ws->nrd.reset();
    m_ws->w = m_ws->h = 0;
    LOG_INFO("RTShadowPass: working set + SIGMA pool released (RT shadows switched off)");
}

void RTShadowPass::OnService(RG::ServiceContext&)
{
    EnsureResources(m_request.wanted);
    for (size_t i = 0; i < m_retiredNrd.size();)
    {
        if (--m_retiredNrd[i].second == 0)
            m_retiredNrd.erase(m_retiredNrd.begin() + static_cast<ptrdiff_t>(i));
        else
            ++i;
    }
    SyncImports();
}

void RTShadowPass::EnsureViewResources(uint32_t w, uint32_t h)
{
    if (m_ws == &m_main || !m_gfx || !m_layout.IsValid()) return;
    if (m_resourcesWanted && m_tracePSO.IsValid())
        BuildWorkingSet(*m_gfx, w, h, /*mayStall=*/false);
    else if (m_ws->final.IsValid() || m_ws->nrd)
    {
        DestroyResolutionResources(*m_gfx);
        RetireNrd();
        m_ws->w = m_ws->h = 0;
    }
    SyncImports();
}

void RTShadowPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
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

void RTShadowPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* tex : { &ws.penumbra, &ws.normalRough, &ws.viewZ, &ws.mv,
                               &ws.translucency, &ws.shadowOut, &ws.final })
        if (tex->IsValid()) gfx.DestroyTexture(*tex);
    if (owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd), RHI::kFramesInFlight + 1u);
    ws = WorkingSet{};
}

void RTShadowPass::OnCompile(IGraphicsDevice& gfx)
{
    if (!m_tracePSO.IsValid()) return;   // not wanted yet (BuildPipelines), failed, or no DXR
    // Enable gate - see RTAOPass::OnCompile.
    if (!m_resourcesWanted) return;
    BuildWorkingSet(gfx, gfx.GetRenderWidth(), gfx.GetRenderHeight(), /*mayStall=*/true);
    SyncImports();
}

void RTShadowPass::BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall)
{
    WorkingSet& ws = *m_ws;
    if (w == 0 || h == 0) return;
    if (w == ws.w && h == ws.h && ws.final.IsValid() && ws.nrd && ws.nrd->IsReady())
    {
        ws.ready = true;   // see RTAOPass::OnCompile
        return;
    }

    DestroyResolutionResources(gfx);
    ws.w = w; ws.h = h;
    RequestHistoryReset();

    // @p tag names the resource too (the barrier journal and the frame capture
    // match textures by their D3D12 name).
    auto make = [&](RHI::Format fmt, RHI::Texture& tex, RHI::ResourceState& st,
                    const char* tag, const void* init = nullptr)
    {
        RHI::TextureDesc td{};
        td.width = w; td.height = h; td.depth = 1; td.mipLevels = 1;
        td.format     = fmt;
        td.debugName = tag;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        RHI::SubresourceData sd{};
        const RHI::SubresourceData* psd = nullptr;
        if (init) { sd = *static_cast<const RHI::SubresourceData*>(init); psd = &sd; }
        if (!gfx.CreateTexture(td, tex, psd))
            LOG_ERROR("RTShadowPass: %s texture creation failed", tag);
        st = RHI::ResourceState::SHADER_RESOURCE;
    };
    make(RHI::Format::R16_FLOAT,         ws.penumbra,    ws.penumbraState,    "RTShadow.penumbra");
    make(RHI::Format::R10G10B10A2_UNORM, ws.normalRough, ws.normalRoughState, "RTShadow.normalRough");
    make(RHI::Format::R32_FLOAT,         ws.viewZ,       ws.viewZState,       "RTShadow.viewZ");
    make(RHI::Format::R16G16_FLOAT,      ws.mv,          ws.mvState,          "RTShadow.mv");
    // IN_TRANSLUCENCY + OUT_SHADOW_TRANSLUCENCY are RGBA8 under
    // SIGMA_SHADOW_TRANSLUCENCY: SIGMA_Config.hlsli defines SIGMA_TYPE as
    // float4 when TRANSLUCENCY==1, and Sigma_ShadowTranslucency.hpp allocates
    // its own transient pool as RGBA8_UNORM. Do not make shadowOut R8: every
    // float4 store would silently drop .yzw - no validation error, just a
    // shadow that ignores translucency.
    make(RHI::Format::R8G8B8A8_UNORM,    ws.translucency, ws.translucencyState, "RTShadow.translucency");
    make(RHI::Format::R8G8B8A8_UNORM,    ws.shadowOut,   ws.shadowOutState,   "RTShadow.shadowOut");
    // Final mask WHITE-initialised: if an early-out ever skips a frame while
    // the LightCB flag is set, Lighting must read "fully lit".
    {
        std::vector<uint8_t> white(size_t(w) * size_t(h) * 4u, 0xFFu);
        RHI::SubresourceData init{ white.data(), w * 4u, uint32_t(white.size()) };
        make(RHI::Format::R8G8B8A8_UNORM, ws.final, ws.finalState, "RTShadow.final", &init);
    }

    // NRD SIGMA_SHADOW_TRANSLUCENCY instance. SetKind MUST precede the first
    // Resize - the nrd::Instance bakes the denoiser at creation. A view's segment
    // cannot drain the GPU before NRD tears its old pool down: its old instance
    // retires and a fresh one is made.
    if (!mayStall) RetireNrd();
    if (!ws.nrd)
    {
        ws.nrd = std::make_unique<NrdDenoiser>();
        ws.nrd->SetKind(NrdDenoiser::Kind::SigmaShadowTranslucency);
    }
    if (!ws.nrd->Resize(gfx, w, h))
    {
        LOG_WARNING("RTShadowPass: NRD SIGMA_SHADOW_TRANSLUCENCY unavailable "
                    "— RT shadows disabled");
        ws.ready = false;
        return;
    }
    ws.ready = true;
}

uint64_t RTShadowPass::GetUpcomingOutputSrv() const
{
    if (!m_ready || !m_ws->ready || !m_gfx || !m_ws->final.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_ws->final);
}

const RHI::Texture* RTShadowPass::GetDebugOutput(RHI::ResourceState& state) const
{
    const WorkingSet& ws = *m_ws;
    if (!m_ready || !ws.ready || !ws.active || !ws.final.IsValid()) return nullptr;
    state = (m_finalH.IsValid() && m_gfx) ? m_gfx->QueryTextureState(ws.final) : ws.finalState;
    return &ws.final;
}

RHI::CommandList RTShadowPass::Execute(RHI::CommandList cl)
{
    WorkingSet& ws = *m_ws;
    if (!m_ready || !ws.ready || !ws.active || ws.fd.tlasVA == 0)
        return cl;
    if (!ws.final.IsValid() || !ws.penumbra.IsValid() || !ws.nrd || !ws.nrd->IsReady())
        return cl;

    IGraphicsDevice& gfx = cl.GetDevice();

    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex    = ctx.GetTexture(m_depthH);
    const RHI::Texture* normalTex   = ctx.GetTexture(m_normalH);
    const RHI::Texture* surfaceTex  = ctx.GetTexture(m_surfaceH);
    const RHI::Texture* velocityTex = ctx.GetTexture(m_velocityH);
    if (!depthTex || !normalTex || !surfaceTex || !velocityTex)
        return cl;
    const uint64_t depthSrv    = gfx.GetTextureSRVGpuHandle(*depthTex);
    const uint64_t normalSrv   = gfx.GetTextureSRVGpuHandle(*normalTex);
    const uint64_t surfaceSrv  = gfx.GetTextureSRVGpuHandle(*surfaceTex);
    const uint64_t velocitySrv = gfx.GetTextureSRVGpuHandle(*velocityTex);
    if (!depthSrv || !normalSrv || !surfaceSrv || !velocitySrv)
        return cl;

    // ---- Fill CB ----
    {
        using namespace DirectX;
        RTShadowConstants cb{};
        const XMMATRIX vp    = XMLoadFloat4x4(&ws.fd.viewProj);
        const XMMATRIX invVP = XMMatrixInverse(nullptr, vp);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), XMMatrixTranspose(invVP));
        cb.sunDirection[0] = ws.fd.sunDirection.x;
        cb.sunDirection[1] = ws.fd.sunDirection.y;
        cb.sunDirection[2] = ws.fd.sunDirection.z;
        cb.sunTanHalfAngle = sunTanHalfAngle;
        cb.screenW = ws.w; cb.screenH = ws.h;
        cb.invScreenW = 1.0f / float(ws.w);
        cb.invScreenH = 1.0f / float(ws.h);
        cb.normalBias = normalBias;
        cb.rayTMax    = rayTMax;
        cb.frameIndex = ws.fd.frameIndex;
        cb.nearZ = ws.fd.nearZ;
        cb.farZ  = ws.fd.farZ;
        // Alpha-cutout gate: all four resources must be live or the shader accepts
        // every candidate (every occluder solid). This is the "table not live this
        // frame" guard, NOT the kill switch - the real switch is p.alphaClip in
        // DDGISceneAS's fillMaterialProxy, because clearing this flag leaves the
        // instance FORCE_NON_OPAQUE and traversal still leaves fixed-function on
        // every candidate.
        cb.alphaTestEnabled = (ws.fd.alphaRecordVA && ws.fd.instanceVA
                               && ws.fd.bindlessBufferTable
                               && m_bindlessTexTable) ? 1u : 0u;
        cb.alphaInstanceCount = ws.fd.alphaInstanceCount;
        cb.alphaMipLevel      = alphaMipLevel;
        if (auto* slot = m_cb.Current(gfx)) *slot = cb;
    }

    const uint32_t gx = (ws.w + 7u) / 8u;
    const uint32_t gy = (ws.h + 7u) / 8u;

    // ---- 1. Trace + pack - writes the SIGMA inputs ----
    TransitionTex(gfx, ws.penumbra,    ws.penumbraState,    RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.normalRough, ws.normalRoughState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.viewZ,       ws.viewZState,       RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.mv,          ws.mvState,          RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.translucency, ws.translucencyState, RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_tracePSO, cl);   // the trace PSO and its layout
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(kTLAS, ws.fd.tlasVA, cl);
    // Unconditional - a root descriptor the shader declares but that is never
    // set is a debug-layer error at Dispatch. VA 0 is what an inert frame
    // already passes; cb.alphaTestEnabled is what keeps the shader off it.
    gfx.SetComputeRootSRVAddress(kAlphaRecords, ws.fd.alphaRecordVA, cl);
    gfx.SetComputeRootSRVAddress(kInstances, ws.fd.instanceVA, cl);
    if (ws.fd.bindlessBufferTable)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, ws.fd.bindlessBufferTable, cl);
    if (m_bindlessTexTable)
        gfx.SetComputeDescriptorTable(kBindlessTex, m_bindlessTexTable, cl);
    gfx.SetComputeDescriptorTable(kSRV_Depth,       depthSrv,    cl);
    gfx.SetComputeDescriptorTable(kSRV_Normal,      normalSrv,   cl);
    gfx.SetComputeDescriptorTable(kSRV_Surface,     surfaceSrv,  cl);
    gfx.SetComputeDescriptorTable(kSRV_Velocity,    velocitySrv, cl);
    gfx.SetComputeDescriptorTable(kUAV_Penumbra,    gfx.GetTextureUAVGpuHandle(ws.penumbra), cl);
    gfx.SetComputeDescriptorTable(kUAV_NormalRough, gfx.GetTextureUAVGpuHandle(ws.normalRough), cl);
    gfx.SetComputeDescriptorTable(kUAV_ViewZ,       gfx.GetTextureUAVGpuHandle(ws.viewZ), cl);
    gfx.SetComputeDescriptorTable(kUAV_MV,          gfx.GetTextureUAVGpuHandle(ws.mv), cl);
    gfx.SetComputeDescriptorTable(kUAV_Translucency, gfx.GetTextureUAVGpuHandle(ws.translucency), cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- 2. NRD SIGMA (inputs NPSR, output UAV) ----
    // NRD restores these exact states before returning, and trashes the heaps and
    // the root signature.
    TransitionTex(gfx, ws.penumbra,    ws.penumbraState,    RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.normalRough, ws.normalRoughState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.viewZ,       ws.viewZState,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.mv,          ws.mvState,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.translucency, ws.translucencyState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.shadowOut,   ws.shadowOutState,   RHI::ResourceState::UNORDERED_ACCESS, cl);

    NrdDenoiser::Params np{};
    std::memcpy(np.viewToClip,  &ws.fd.projMatrixNoJitter, sizeof(np.viewToClip));
    std::memcpy(np.worldToView, &ws.fd.viewMatrix,         sizeof(np.worldToView));
    np.jitter[0] = ws.fd.jitterX; np.jitter[1] = ws.fd.jitterY;
    np.frameIndex     = ws.fd.frameIndex;
    np.resetHistory   = !m_hist || m_hist->reset;
    np.denoisingRange = ws.fd.farZ * 0.99f;
    np.width = ws.w; np.height = ws.h;
    // SIGMA needs the world-space direction TO the sun (lightDir is from-light).
    np.lightDirection[0] = -ws.fd.sunDirection.x;
    np.lightDirection[1] = -ws.fd.sunDirection.y;
    np.lightDirection[2] = -ws.fd.sunDirection.z;

    auto bind = [&](nrd::ResourceType type, const RHI::Texture& t, RHI::ResourceState st) {
        NrdDenoiser::Binding b;
        b.type = type; b.tex.texture = &t; b.tex.state = st;
        return b;
    };
    const NrdDenoiser::Binding sigmaBindings[] = {
        bind(nrd::ResourceType::IN_MV,                   ws.mv,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS,     ws.normalRough, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_VIEWZ,                ws.viewZ,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_PENUMBRA,             ws.penumbra,    RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        // SIGMA_SHADOW_TRANSLUCENCY only. NrdDenoiser aborts the WHOLE denoise
        // if a dispatch wants an unbound resource type, so this row and the
        // count below must land together with the Kind switch.
        bind(nrd::ResourceType::IN_TRANSLUCENCY,         ws.translucency, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, ws.shadowOut,   RHI::ResourceState::UNORDERED_ACCESS),
    };
    ws.nrd->Denoise(gfx, cl, np, sigmaBindings, 6);

    // ---- 3. NRD trashed heaps + root sig - restore ours for the resolve ----
    gfx.BindDescriptorHeaps(cl);
    gfx.BindComputePipelineState(m_resolvePSO, cl);   // the resolve PSO, and our layout back
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    // Setting the layout invalidated EVERY root argument. The resolve shader only
    // touches kCB / kSRV_ShadowIn / kSRV_DepthR / kUAV_Final, but the material
    // params are rebound too so a material-aware resolve does not inherit an
    // unbound-table read.
    gfx.SetComputeRootSRVAddress(kAlphaRecords, ws.fd.alphaRecordVA, cl);
    gfx.SetComputeRootSRVAddress(kInstances, ws.fd.instanceVA, cl);
    if (ws.fd.bindlessBufferTable)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, ws.fd.bindlessBufferTable, cl);
    if (m_bindlessTexTable)
        gfx.SetComputeDescriptorTable(kBindlessTex, m_bindlessTexTable, cl);

    // ---- 4. Resolve SIGMA output -> the final mask (x*x) ----
    // With the mask imported the graph transitions it (to UAV in this pass's
    // Phase A, back for each reader); these are the pass's own only without the
    // import (-rgimportoff).
    const bool graphTracked = m_finalH.IsValid();
    TransitionTex(gfx, ws.shadowOut, ws.shadowOutState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    if (!graphTracked)
        TransitionTex(gfx, ws.final, ws.finalState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    gfx.SetComputeDescriptorTable(kSRV_ShadowIn, gfx.GetTextureSRVGpuHandle(ws.shadowOut), cl);
    gfx.SetComputeDescriptorTable(kSRV_DepthR,   depthSrv, cl);
    gfx.SetComputeDescriptorTable(kUAV_Final,    gfx.GetTextureUAVGpuHandle(ws.final), cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- Back to sampleable for Lighting.ps (pixel-shader read) ----
    if (!graphTracked)
        TransitionTex(gfx, ws.final, ws.finalState, RHI::ResourceState::SHADER_RESOURCE, cl);

    if (m_hist) m_hist->reset = false;
    return cl;
}
