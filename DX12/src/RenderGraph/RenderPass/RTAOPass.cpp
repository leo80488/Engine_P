#include "RenderGraph/RenderPass/RTAOPass.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/MeshDescriptorHeap.h"   // kMaxBuffers (bindless VB/IB table)
#include "Graphics/NrdDenoiser.h"          // REBLUR_DIFFUSE_OCCLUSION denoiser
#include "RenderGraph/PassTextureNames.h"
#include "System/Log.h"

#include <chrono>
#include <vector>
#include <string>
#include <iterator>
#include <cstring>

namespace
{
// Shader compiles go through the shared disk cache (Graphics/ShaderDiskCache.h):
// ShaderLibrary's .ishdr cache in shaders/shader_cache_dxil/, keyed on path + entry + stage +
// defines + compile flags and invalidated by the mtime of the source or any transitive
// #include. Do not compile uncached here: a full DXC run per boot cost seconds per pass.
std::vector<uint8_t> CompileShaderFile(const std::string& path, const std::string& entry)
{
    return ShaderDiskCache::LoadOrCompile(path, entry, RHI::ShaderStage::CS, {}, "RTAOPass");
}

// Root parameter indices - keep in sync with RTAO.cs.hlsl + RTAOResolve.cs.hlsl. The REBLUR
// input / output textures cross into NRD through NrdDenoiser::Denoise (its own root
// signature), not this one.
constexpr RHI::Slots::CmpParam kCB              { 0 };   // b0
constexpr RHI::Slots::CmpParam kTLAS           { 1 };   // t0 (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kSRV_Depth      { 2 };   // t1
constexpr RHI::Slots::CmpParam kSRV_Normal     { 3 };   // t2
constexpr RHI::Slots::CmpParam kSRV_Velocity   { 4 };   // t3
constexpr RHI::Slots::CmpParam kSRV_AoIn       { 5 };   // t4 (resolve: REBLUR output)
constexpr RHI::Slots::CmpParam kUAV_DiffHit    { 6 };   // u0
constexpr RHI::Slots::CmpParam kUAV_NormalRough{ 7 };   // u1
constexpr RHI::Slots::CmpParam kUAV_ViewZ      { 8 };   // u2
constexpr RHI::Slots::CmpParam kUAV_MV         { 9 };   // u3
constexpr RHI::Slots::CmpParam kUAV_AoOut      { 10 };  // u4
// Alpha-cutout occluders - see RTShadowPass for the identical expansion.
constexpr RHI::Slots::CmpParam kAlphaRecords   { 11 };  // t5 space0 (root SRV, raw VA)
constexpr RHI::Slots::CmpParam kBindlessBuffers{ 12 };  // t0.. space1
constexpr RHI::Slots::CmpParam kBindlessTex    { 13 };  // t0.. space2
// Per-instance data (RTInstanceData, 128 B), bound only so the candidate cutout test can
// apply the material UV transform, which the compact 40 B alpha record does not carry. Same
// InstanceID() index space as kAlphaRecords, gated by the same cb.alphaTestEnabled.
constexpr RHI::Slots::CmpParam kInstances      { 14 };  // t6 space0 (root SRV, raw VA)
constexpr uint32_t kNumRootParams  = 15;

void TransitionTex(IGraphicsDevice& gfx, const RHI::Texture& tex,
                   RHI::ResourceState& state, RHI::ResourceState to, RHI::CommandList cl)
{
    if (state == to || !tex.IsValid()) return;
    gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, state, to), cl);
    state = to;
}
} // namespace

RTAOPass::RTAOPass(RG::RGTextureHandle normal,
                   RG::RGTextureHandle depth,
                   RG::RGTextureHandle velocity)
    : m_normalH(normal), m_depthH(depth), m_velocityH(velocity)
{
}

// The resolved AO (R8, the render size) as this pass's import: the graph reads the member at
// each Execute and transitions it (ImportTracking::Graph) - UNORDERED_ACCESS in this pass's
// Phase A, SHADER_RESOURCE in each reader's.
void RTAOPass::DeclareImports(RG::ImportDeclarer& d)
{
    m_aoFinalH = d.Import(RG::PassTex::kRTAO, { RHI::Format::R8_UNORM, 0, 0, false, true },
                          { &m_aoFinalImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
}

void RTAOPass::Setup(RG::RenderGraphBuilder& b)
{
    b.ReadSRV(m_normalH);
    b.ReadSRV(m_depthH);
    b.ReadSRV(m_velocityH);
    b.WriteUAV(m_aoFinalH);   // the graph brings the AO to UNORDERED_ACCESS
}

void RTAOPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    if (!gfx.GetCapabilities().inlineRaytracing)
    {
        LOG_INFO("RTAOPass: device does not support DXR — pass disabled");
        m_ready = false;
        return;
    }
    if (!BuildRootSignature(gfx)) return;
    // The pipelines are NOT built here: BuildPipelines, the first time RTAO is
    // wanted (EnsureResources).
    m_bindlessTexTable = gfx.GetBindlessTextureTableGpuHandle();
    if (!m_cb.Create(gfx, "RTAO.CB"))
    {
        LOG_ERROR("RTAOPass: CB creation failed");
        return;
    }
    m_ready = true;   // finalised in OnCompile once NRD is up
    LOG_INFO("RTAOPass: initialised (inline RayQuery + NRD REBLUR_DIFFUSE_OCCLUSION)");
}

void RTAOPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_layout.IsValid()) return;   // no DXR, or Init failed
    gfx.DestroyPipelineState(m_tracePSO);
    gfx.DestroyPipelineState(m_resolvePSO);
    m_pipelinesFailed = false;
    // Rebuild the root signature too: otherwise a hot reload after a root-signature edit feeds
    // the new shader to the old signature, CreateComputePipelineState fails, and the pass
    // disables itself for the rest of the session.
    gfx.DestroyPipelineLayout(m_layout);
    if (!BuildRootSignature(gfx))
    {
        LOG_ERROR("RTAOPass: root signature rebuild failed — pass disabled");
        m_ready = false;
        return;
    }
    // In use: rebuilt now, not a frame late. Otherwise the next EnsureResources(true) builds them
    // from the new source, so re-enabling RTAO never runs stale PSOs.
    if (m_resourcesWanted && !BuildPipelines("shader reload")) return;
    RequestHistoryReset();
}

bool RTAOPass::BuildPipelines(const char* why)
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
        LOG_ERROR("RTAOPass: pipelines failed — pass disabled until a shader reload");
        return false;
    }
    LOG_INFO("RTAOPass: pipelines built on %s (%.0f ms)", why,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

bool RTAOPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    RHI::PipelineLayoutBuilder b("RTAO");
    b.RootCBV(kCB, 0)
     .RootSRV(kTLAS, 0)            // t0 - TLAS
     // Per-instance alpha-cutout records (RTAlphaRecord, 40 B).
     .RootSRV(kAlphaRecords, 5)    // t5
     // Instance table - supplies the material UV transform to the cutout test.
     .RootSRV(kInstances, 6)       // t6
     .Table(kSRV_Depth,        { RHI::SrvRange(1) })
     .Table(kSRV_Normal,       { RHI::SrvRange(2) })
     .Table(kSRV_Velocity,     { RHI::SrvRange(3) })
     .Table(kSRV_AoIn,         { RHI::SrvRange(4) })
     .Table(kUAV_DiffHit,      { RHI::UavRange(0) })
     .Table(kUAV_NormalRough,  { RHI::UavRange(1) })
     .Table(kUAV_ViewZ,        { RHI::UavRange(2) })
     .Table(kUAV_MV,           { RHI::UavRange(3) })
     .Table(kUAV_AoOut,        { RHI::UavRange(4) })
     // The two bindless tables the alpha-cutout candidate test walks - each
     // range as wide as its heap (a 1-descriptor table here would read out of
     // bounds for every index > 0 with no debug-layer error).
     .Table(kBindlessBuffers,  { RHI::SrvRange(0, MeshDescriptorHeap::kMaxBuffers, 1) })
     .Table(kBindlessTex,      { RHI::SrvRange(0, GraphicsDX12::kMaxBindlessTextures, 2) });
    // s1 - linear WRAP. MaxLOD stays FLT_MAX (SamplerDesc's default): zero
    // clamps every SampleLevel to mip 0 and kills the alphaMipLevel knob.
    RHI::SamplerDesc linearWrap{};
    linearWrap.maxAnisotropy = 1;
    b.Sampler(linearWrap, 1);
    // HEAP_DIRECTLY_INDEXED like the shared graphics / compute signatures, DDGI, RTR and the RTDI
    // shell, so whether a shader may open ResourceDescriptorHeap[] does not depend on which pass
    // binds it.
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("RTAOPass: pipeline layout failed");
        return false;
    }
    return true;
}

bool RTAOPass::CreatePipelines(IGraphicsDevice& gfx)
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
        if (!ok) LOG_ERROR("RTAOPass: pipeline failed for %s", path.c_str());
        return ok;
    };
    if (!buildCS("shaders/RTAO.cs.hlsl",        m_tracePSO))   return false;
    if (!buildCS("shaders/RTAOResolve.cs.hlsl", m_resolvePSO)) return false;
    return true;
}

void RTAOPass::DestroyResolutionResources(IGraphicsDevice& gfx)
{
    WorkingSet& ws = *m_ws;
    for (RHI::Texture* tex : { &ws.diffHitIn, &ws.normalRough, &ws.viewZ, &ws.mv, &ws.diffHitOut, &ws.aoFinal })
        if (tex->IsValid()) gfx.DestroyTexture(*tex);
    ws.diffHitIn = {}; ws.normalRough = {}; ws.viewZ = {}; ws.mv = {}; ws.diffHitOut = {}; ws.aoFinal = {};
    ws.ready = false;
}

void RTAOPass::RetireNrd()
{
    if (m_ws->nrd) m_retiredNrd.emplace_back(std::move(m_ws->nrd), RHI::kFramesInFlight + 1u);
}

void RTAOPass::EnsureResources(bool wanted)
{
    m_resourcesWanted = wanted;
    if (!m_gfx || !m_layout.IsValid()) return;   // no DXR, or Init failed

    if (wanted)
    {
        // The pipelines first (built once). Idempotent - OnCompile early-outs when sized and NRD is
        // ready, so the steady-state cost of this per-frame call is a few compares.
        if (BuildPipelines("first use")) OnCompile(*m_gfx);
        return;
    }
    if (!m_ws->aoFinal.IsValid() && !m_ws->nrd) return;   // already released

    // Feature switched off: NrdDenoiser tears its pool down immediately (no deferral), so drain
    // the GPU first - Resize's convention; a settings toggle is rarer than a resize, so the
    // one-off stall is fine. The frame boundary: the main view is bound.
    m_gfx->WaitIdleAndReleaseDeferred();
    DestroyResolutionResources(*m_gfx);
    m_ws->nrd.reset();
    m_ws->w = m_ws->h = 0;
    LOG_INFO("RTAOPass: working set + NRD pool released (RTAO switched off)");
}

void RTAOPass::OnService(RG::ServiceContext&)
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

void RTAOPass::EnsureViewResources(uint32_t w, uint32_t h)
{
    if (m_ws == &m_main || !m_gfx || !m_layout.IsValid()) return;
    if (m_resourcesWanted && m_tracePSO.IsValid())
        BuildWorkingSet(*m_gfx, w, h, /*mayStall=*/false);
    else if (m_ws->aoFinal.IsValid() || m_ws->nrd)
    {
        DestroyResolutionResources(*m_gfx);
        RetireNrd();
        m_ws->w = m_ws->h = 0;
    }
    SyncImports();
}

void RTAOPass::BindView(ViewState& view)
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

void RTAOPass::ViewWork::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* tex : { &ws.diffHitIn, &ws.normalRough, &ws.viewZ, &ws.mv, &ws.diffHitOut, &ws.aoFinal })
        if (tex->IsValid()) gfx.DestroyTexture(*tex);
    if (owner && ws.nrd) owner->m_retiredNrd.emplace_back(std::move(ws.nrd), RHI::kFramesInFlight + 1u);
    ws = WorkingSet{};
}

void RTAOPass::OnCompile(IGraphicsDevice& gfx)
{
    if (!m_tracePSO.IsValid()) return;   // not wanted yet (BuildPipelines), failed, or no DXR
    // Enable gate: boot and resolution-change Compiles reach every registered pass - a disabled
    // RTAO must not pay ~30 MiB + an NRD pool for textures nothing reads (EnsureResources owns
    // the flag).
    if (!m_resourcesWanted) return;
    BuildWorkingSet(gfx, gfx.GetRenderWidth(), gfx.GetRenderHeight(), /*mayStall=*/true);
    SyncImports();
}

void RTAOPass::BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall)
{
    WorkingSet& ws = *m_ws;
    if (w == 0 || h == 0) return;
    if (w == ws.w && h == ws.h && ws.aoFinal.IsValid() && ws.nrd && ws.nrd->IsReady())
    {
        // Sized and denoiser up: ready - also after a shader reload rebuilt the
        // pipelines that a failed one had cleared.
        ws.ready = true;
        return;
    }

    DestroyResolutionResources(gfx);
    ws.w = w; ws.h = h;
    RequestHistoryReset();

    // @p tag names the resource too: the barrier journal and the frame capture match textures by
    // their D3D12 name.
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
        const RHI::SubresourceData* psd = static_cast<const RHI::SubresourceData*>(init);
        if (!gfx.CreateTexture(td, tex, psd))
            LOG_ERROR("RTAOPass: %s texture creation failed", tag);
        st = RHI::ResourceState::SHADER_RESOURCE;
    };
    make(RHI::Format::R8_UNORM,          ws.diffHitIn,   ws.diffHitInState,   "RTAO.diffHitIn");
    make(RHI::Format::R10G10B10A2_UNORM, ws.normalRough, ws.normalRoughState, "RTAO.normalRough");
    make(RHI::Format::R32_FLOAT,         ws.viewZ,       ws.viewZState,       "RTAO.viewZ");
    make(RHI::Format::R16G16_FLOAT,      ws.mv,          ws.mvState,          "RTAO.mv");
    make(RHI::Format::R8_UNORM,          ws.diffHitOut,  ws.diffHitOutState,  "RTAO.diffHitOut");
    // Final AO WHITE-initialised (1 = no occlusion): if an early-out ever
    // skips a frame while SetSSAOHandle already points here, Lighting reads
    // "no occlusion", never uninitialised VRAM.
    {
        std::vector<uint8_t> white(size_t(w) * size_t(h), 0xFFu);
        RHI::SubresourceData init{ white.data(), w, uint32_t(white.size()) };
        make(RHI::Format::R8_UNORM, ws.aoFinal, ws.aoFinalState, "RTAO.aoFinal", &init);
    }

    // A view's segment cannot drain the GPU before NRD tears its old pool down: its old instance
    // retires and a fresh one is made.
    if (!mayStall) RetireNrd();
    if (!ws.nrd)
    {
        ws.nrd = std::make_unique<NrdDenoiser>();
        ws.nrd->SetKind(NrdDenoiser::Kind::ReblurDiffuseOcclusion);
    }
    if (!ws.nrd->Resize(gfx, w, h))
    {
        LOG_WARNING("RTAOPass: NRD REBLUR_DIFFUSE_OCCLUSION unavailable — RTAO disabled");
        ws.ready = false;
        return;
    }
    ws.ready = true;
}

uint64_t RTAOPass::GetUpcomingOutputSrv() const
{
    if (!m_ready || !m_ws->ready || !m_gfx || !m_ws->aoFinal.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_ws->aoFinal);
}

const RHI::Texture* RTAOPass::GetDebugOutput(RHI::ResourceState& state) const
{
    const WorkingSet& ws = *m_ws;
    if (!m_ready || !ws.ready || !ws.active || !ws.aoFinal.IsValid()) return nullptr;
    state = (m_aoFinalH.IsValid() && m_gfx) ? m_gfx->QueryTextureState(ws.aoFinal) : ws.aoFinalState;
    return &ws.aoFinal;
}

RHI::CommandList RTAOPass::Execute(RHI::CommandList cl)
{
    WorkingSet& ws = *m_ws;
    if (!m_ready || !ws.ready || !ws.active || ws.fd.tlasVA == 0)
        return cl;
    if (!ws.aoFinal.IsValid() || !ws.diffHitIn.IsValid() || !ws.nrd || !ws.nrd->IsReady())
        return cl;

    IGraphicsDevice& gfx = cl.GetDevice();

    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex    = ctx.GetTexture(m_depthH);
    const RHI::Texture* normalTex   = ctx.GetTexture(m_normalH);
    const RHI::Texture* velocityTex = ctx.GetTexture(m_velocityH);
    if (!depthTex || !normalTex || !velocityTex)
        return cl;
    const uint64_t depthSrv    = gfx.GetTextureSRVGpuHandle(*depthTex);
    const uint64_t normalSrv   = gfx.GetTextureSRVGpuHandle(*normalTex);
    const uint64_t velocitySrv = gfx.GetTextureSRVGpuHandle(*velocityTex);
    if (!depthSrv || !normalSrv || !velocitySrv)
        return cl;

    // ---- Fill CB ----
    {
        using namespace DirectX;
        RTAOConstants cb{};
        const XMMATRIX vp    = XMLoadFloat4x4(&ws.fd.viewProj);
        const XMMATRIX invVP = XMMatrixInverse(nullptr, vp);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj), XMMatrixTranspose(invVP));
        cb.screenW = ws.w; cb.screenH = ws.h;
        cb.invScreenW = 1.0f / float(ws.w);
        cb.invScreenH = 1.0f / float(ws.h);
        cb.aoRadius   = aoRadius;
        cb.normalBias = normalBias;
        cb.rayCount   = rayCount;
        cb.frameIndex = ws.fd.frameIndex;
        cb.nearZ = ws.fd.nearZ;
        cb.farZ  = ws.fd.farZ;
        cb.aoPower = aoPower;
        // Alpha-cutout gate - see RTShadowPass. This is the "table not live" guard, not the kill
        // switch (that is p.alphaClip in DDGISceneAS).
        cb.alphaTestEnabled = (ws.fd.alphaRecordVA && ws.fd.instanceVA
                               && ws.fd.bindlessBufferTable
                               && m_bindlessTexTable) ? 1u : 0u;
        cb.alphaInstanceCount = ws.fd.alphaInstanceCount;
        cb.alphaMipLevel      = alphaMipLevel;
        if (auto* slot = m_cb.Current(gfx)) *slot = cb;
    }

    const uint32_t gx = (ws.w + 7u) / 8u;
    const uint32_t gy = (ws.h + 7u) / 8u;

    // ---- 1. Trace + pack: writes the REBLUR occlusion inputs ----
    TransitionTex(gfx, ws.diffHitIn,   ws.diffHitInState,   RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.normalRough, ws.normalRoughState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.viewZ,       ws.viewZState,       RHI::ResourceState::UNORDERED_ACCESS, cl);
    TransitionTex(gfx, ws.mv,          ws.mvState,          RHI::ResourceState::UNORDERED_ACCESS, cl);

    gfx.BindComputePipelineState(m_tracePSO, cl);   // the trace PSO and its layout
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeRootSRVAddress(kTLAS, ws.fd.tlasVA, cl);
    // Unconditional: a root descriptor the shader declares but that is never set is a
    // debug-layer error at Dispatch. cb.alphaTestEnabled is the gate.
    gfx.SetComputeRootSRVAddress(kAlphaRecords, ws.fd.alphaRecordVA, cl);
    gfx.SetComputeRootSRVAddress(kInstances, ws.fd.instanceVA, cl);
    if (ws.fd.bindlessBufferTable)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, ws.fd.bindlessBufferTable, cl);
    if (m_bindlessTexTable)
        gfx.SetComputeDescriptorTable(kBindlessTex, m_bindlessTexTable, cl);
    gfx.SetComputeDescriptorTable(kSRV_Depth,       depthSrv,    cl);
    gfx.SetComputeDescriptorTable(kSRV_Normal,      normalSrv,   cl);
    gfx.SetComputeDescriptorTable(kSRV_Velocity,    velocitySrv, cl);
    gfx.SetComputeDescriptorTable(kUAV_DiffHit,     gfx.GetTextureUAVGpuHandle(ws.diffHitIn), cl);
    gfx.SetComputeDescriptorTable(kUAV_NormalRough, gfx.GetTextureUAVGpuHandle(ws.normalRough), cl);
    gfx.SetComputeDescriptorTable(kUAV_ViewZ,       gfx.GetTextureUAVGpuHandle(ws.viewZ), cl);
    gfx.SetComputeDescriptorTable(kUAV_MV,          gfx.GetTextureUAVGpuHandle(ws.mv), cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- 2. NRD REBLUR_DIFFUSE_OCCLUSION ----
    TransitionTex(gfx, ws.diffHitIn,   ws.diffHitInState,   RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.normalRough, ws.normalRoughState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.viewZ,       ws.viewZState,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.mv,          ws.mvState,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    TransitionTex(gfx, ws.diffHitOut,  ws.diffHitOutState,  RHI::ResourceState::UNORDERED_ACCESS, cl);

    NrdDenoiser::Params np{};
    std::memcpy(np.viewToClip,  &ws.fd.projMatrixNoJitter, sizeof(np.viewToClip));
    std::memcpy(np.worldToView, &ws.fd.viewMatrix,         sizeof(np.worldToView));
    np.jitter[0] = ws.fd.jitterX; np.jitter[1] = ws.fd.jitterY;
    np.frameIndex     = ws.fd.frameIndex;
    np.resetHistory   = !m_hist || m_hist->reset;
    np.denoisingRange = ws.fd.farZ * 0.99f;
    np.width = ws.w; np.height = ws.h;
    // AO normalization A - must equal the pack shader's kAOHitDistParams.x (= aoRadius) so a miss
    // maps to 1 = unoccluded.
    np.aoDistanceScale = aoRadius;

    auto bind = [&](nrd::ResourceType type, const RHI::Texture& t, RHI::ResourceState st) {
        NrdDenoiser::Binding b;
        b.type = type; b.tex.texture = &t; b.tex.state = st;
        return b;
    };
    const NrdDenoiser::Binding aoBindings[] = {
        bind(nrd::ResourceType::IN_MV,               ws.mv,          RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_NORMAL_ROUGHNESS, ws.normalRough, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_VIEWZ,            ws.viewZ,       RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::IN_DIFF_HITDIST,     ws.diffHitIn,   RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
        bind(nrd::ResourceType::OUT_DIFF_HITDIST,    ws.diffHitOut,  RHI::ResourceState::UNORDERED_ACCESS),
    };
    ws.nrd->Denoise(gfx, cl, np, aoBindings, 5);

    // ---- 3. NRD replaced the heaps and the root signature: restore them for the resolve ----
    gfx.BindDescriptorHeaps(cl);
    gfx.BindComputePipelineState(m_resolvePSO, cl);   // the resolve PSO, and our layout back
    gfx.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(gfx), 0, cl);
    // Setting the layout invalidated every root argument: rebind the material params so a
    // material-aware resolve cannot read unbound ones. Unconditional: a root descriptor the shader
    // declares but that is never set is a debug-layer error at Dispatch. cb.alphaTestEnabled is
    // the gate.
    gfx.SetComputeRootSRVAddress(kAlphaRecords, ws.fd.alphaRecordVA, cl);
    gfx.SetComputeRootSRVAddress(kInstances, ws.fd.instanceVA, cl);
    if (ws.fd.bindlessBufferTable)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, ws.fd.bindlessBufferTable, cl);
    if (m_bindlessTexTable)
        gfx.SetComputeDescriptorTable(kBindlessTex, m_bindlessTexTable, cl);

    // ---- 4. Resolve the REBLUR output (x aoPower) -> the gSSAO texture ----
    // With the AO imported the graph transitions it (to UAV in this pass's Phase A, back for each
    // reader); TransitionTex handles it only without the import (-rgimportoff).
    const bool graphTracked = m_aoFinalH.IsValid();
    TransitionTex(gfx, ws.diffHitOut, ws.diffHitOutState, RHI::ResourceState::SHADER_RESOURCE_COMPUTE, cl);
    if (!graphTracked)
        TransitionTex(gfx, ws.aoFinal, ws.aoFinalState, RHI::ResourceState::UNORDERED_ACCESS, cl);
    gfx.SetComputeDescriptorTable(kSRV_AoIn,  gfx.GetTextureSRVGpuHandle(ws.diffHitOut), cl);
    gfx.SetComputeDescriptorTable(kUAV_AoOut, gfx.GetTextureUAVGpuHandle(ws.aoFinal), cl);
    gfx.DispatchCompute(gx, gy, 1, cl);

    // ---- Back to sampleable for Lighting.ps (pixel-shader read, t18) ----
    if (!graphTracked)
        TransitionTex(gfx, ws.aoFinal, ws.aoFinalState, RHI::ResourceState::SHADER_RESOURCE, cl);

    if (m_hist) m_hist->reset = false;
    return cl;
}
