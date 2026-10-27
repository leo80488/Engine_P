#include "RenderGraph/RenderPass/TrailPasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadingStencil.h"
#include "Graphics/TrailSystem.h"
#include "RenderGraph/PassTextureNames.h"
#include "RenderGraph/RenderContext.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <cstring>

// Root-parameter indices come from Graphics/RootSignatureLayout.h; the local
// names are aliases, never new numbers.
namespace S    = RHI::Slots::Gfx;      // ABSOLUTE graphics root indices
namespace SRel = RHI::Slots::GfxRel;   // GROUP-RELATIVE (BindConstantBuffer)
namespace C    = RHI::Slots::Cmp;      // ABSOLUTE compute root indices

// Compute root sig slots (the compute root sig is shared; same as ParticlePasses).
static constexpr auto kCSCBSlot       = C::CB;      // b0 space2
static constexpr auto kCSRequestSRV   = C::SRV_t0;  // t0 space2 DESC_TABLE
static constexpr auto kCSSegmentUAV   = C::UAV_u0;  // u0 space2 DESC_TABLE
static constexpr auto kCSHeaderUAV    = C::UAV_u1;  // u1 space2 DESC_TABLE

// Graphics root sig slots (same as ParticlePasses render pass).
static constexpr auto kGfxSegmentSRVSlot  = S::SRV_t4;      // DESC_TABLE t4 space0
static constexpr auto kGfxHeaderSRVSlot   = S::SRV_t5;      // DESC_TABLE t5 space0
static constexpr auto kGfxBindlessTexSlot = S::BindlessTex; // DESC_TABLE t0 space2 - bindless texture array

// Stencil bit for TAA exclusion: the one ParticlePasses stamps
// (Graphics/ShadingStencil.h).
static constexpr uint32_t kTrailTAAExcludeBit = ShadingStencil::kTaaExclude;   // 0x40

// ==== TrailUpdatePass ====

void TrailUpdatePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TrailUpdate_CS, RHI::ShaderStage::CS,
                         "TrailUpdate.cs.hlsl", "CSMain");
    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("TrailUpdatePass: initialised");
}

// Creates into the live member; a failed rebuild keeps the old pipeline.
bool TrailUpdatePass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::TrailUpdate_CS);
    if (!cs)
    {
        LOG_ERROR("TrailUpdatePass: shader lookup failed");
        return false;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("TrailUpdatePass: PSO creation failed");
    return true;
}

void TrailUpdatePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void TrailUpdatePass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid()) return;
    auto& gfx = *m_gfx;

    // One thread per trail: each ages its trail, then replays this frame's requests
    // for its slot in buffer order (TrailUpdate.cs.hlsl). Do not widen this to
    // max(maxTrails, requestCount): per-request threads race the header's head/count
    // once a trail gets > 1 request per frame (CPU slerp sub-sampling).
    const uint32_t threads = TrailSystem::kMaxTrails;

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(*m_gfx), 0, cl);

    const uint64_t reqSrv = m_gfx->GetBufferSRVGpuHandle(m_sys->GetRequestBuffer(*m_gfx));
    gfx.SetComputeDescriptorTable(kCSRequestSRV, reqSrv, cl);
    gfx.SetComputeDescriptorTable(kCSSegmentUAV, m_sys->GetSegmentUAVHandle(), cl);
    gfx.SetComputeDescriptorTable(kCSHeaderUAV,  m_sys->GetHeaderUAVHandle(),  cl);

    gfx.DispatchCompute((threads + 63) / 64, 1, 1, cl);

    // UAV barriers so the render pass sees consistent state.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetSegmentBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetHeaderBuffer()),  cl);
}

// ==== TrailRenderPass ====

TrailRenderPass::TrailRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void TrailRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    // Planes on: depth tested and the stencil plane written (the TAA-exclude bit, the
    // TaaExclude field in RenderGraph/StencilBits.h). -rgplaneoff: a plain
    // depth-stencil write.
    if (RG::RenderGraph::PlanesEnabled())
        b.DepthStencil(m_depth, RG::Depth::Read, RG::Stencil::Write, ShadingStencil::kTaaExclude);
    else                                  b.WriteDepthStencil(m_depth);
    // The soft fade reads the opaque depth snapshot through a heap index
    // (FogApplyCB); the declaration is the contract.
    if (m_opaqueDepth.IsValid()) b.ReadSRV(m_opaqueDepth);
    // The HDR scene colour: the scene snapshot is a scoped read at RENDER_TARGET
    // (CopyHdrSceneToScoped), then the trails draw on top - declared, bound in
    // Execute; Phase A never binds or clears it.
    b.ReadScoped(b.HdrScene(), RHI::ResourceState::RENDERTARGET);
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The froxel fog FogApply samples (VolumetricFogPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kVolFogScattering });
}

void TrailRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Trail_VS, RHI::ShaderStage::VS, "Trail.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Trail_PS, RHI::ShaderStage::PS, "Trail.ps.hlsl", "main");

    const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Trail_VS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Trail_PS);
    if (!vs || !ps)
    {
        LOG_ERROR("TrailRenderPass: shader lookup failed");
        return;
    }

    // PSO permutations (engine PS + per-trail custom PS x stencil variant) come from a
    // pass-local PSOCache, as in ParticleRenderPass. Warm the default (engine PS, no
    // stencil) so the common path never waits.
    m_psoCache.Init(gfx, m_shaderLib);
    if (!m_psoCache.GetOrCreate(BuildTrailPSODesc(0, /*taaExclude=*/false)))
    {
        LOG_ERROR("TrailRenderPass: default PSO creation failed");
        return;
    }

    if (!m_renderCB.Create(gfx, "TrailRenderPass.RenderCB"))
        LOG_ERROR("TrailRenderPass: render CB create failed");
    // TrailFX samples the swipe texture with the fixed SMP_LINEAR_WRAP heap slot, the
    // scene snapshot and the fog volume with SMP_LINEAR_CLAMP, so the pass creates no
    // sampler.

    LOG_SUCCESS("TrailRenderPass: initialised");
}

// Ribbon PSO; the header states the premultiplied-blend contract.
PSODesc TrailRenderPass::BuildTrailPSODesc(uint32_t customPSID, bool taaExclude) const
{
    PSODesc desc;
    desc.vsID = ShaderID::Trail_VS;
    desc.psID = (customPSID != 0) ? static_cast<ShaderID>(customPSID)
                                  : ShaderID::Trail_PS;
    desc.inputLayout = InputLayoutType::None;   // procedural ribbon (SV_VertexID)

    desc.rs.fillMode         = RHI::FillMode::SOLID;
    desc.rs.cullMode         = RHI::CullMode::NONE;   // ribbons are 2-sided
    desc.rs.depthClipEnable = true;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;  // reversed-Z
    if (taaExclude)
    {
        // Stamp the stencil bit on every depth-passing trail pixel (depth write stays
        // off), as ParticlePasses does; the ref comes from SetStencilRef.
        desc.dss.stencilEnable     = true;
        desc.dss.stencilReadMask  = 0xFF;
        desc.dss.stencilWriteMask = kTrailTAAExcludeBit;
        desc.dss.frontFace.stencilFunc          = RHI::ComparisonFunc::ALWAYS;
        desc.dss.frontFace.stencilPassOp       = RHI::StencilOp::REPLACE;
        desc.dss.frontFace.stencilFailOp       = RHI::StencilOp::KEEP;
        desc.dss.frontFace.stencilDepthFailOp = RHI::StencilOp::KEEP;
        desc.dss.backFace = desc.dss.frontFace;
    }

    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable    = true;
    rt.srcBlend       = RHI::Blend::ONE;
    rt.destBlend      = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp        = RHI::BlendOp::ADD;
    rt.srcBlendAlpha = RHI::Blend::ONE;
    rt.destBlendAlpha= RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha  = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.topology      = RHI::PrimitiveTopology::TRIANGLELIST;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
    desc.sampleCount   = 1;
    return desc;
}

void TrailRenderPass::BindView(ViewState& view)
{
    m_scene = view.Slot() == 0 ? &m_mainScene : &view.Data<ViewSnapshot>(this).snapshot;
}

void TrailRenderPass::ViewSnapshot::Release(IGraphicsDevice& gfx)
{
    if (snapshot.tex.IsValid()) gfx.DestroyTexture(snapshot.tex);
    snapshot = SceneSnapshot{};
}

void TrailRenderPass::PrepareSceneSnapshot()
{
    SceneSnapshot& s = *m_scene;
    s.ready = false;
    if (!m_gfx || !m_sys) return;
    bool hasCustom = false;
    for (const auto& bucket : m_sys->GetDrawBuckets())
        hasCustom |= bucket.shaderBucket != 0;
    if (!hasCustom) return;
    const uint32_t w = m_gfx->GetRenderWidth(), h = m_gfx->GetRenderHeight();
    if (!w || !h) return;
    if (!s.tex.IsValid() || s.width != w || s.height != h)
    {
        if (s.tex.IsValid()) m_gfx->DestroyTexture(s.tex);
        RHI::TextureDesc desc{};
        desc.width = w;
        desc.height = h;
        desc.format = RHI::Format::R16G16B16A16_FLOAT;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.usage = RHI::Usage::DEFAULT;
        desc.layout = RHI::ResourceState::COPY_DST;
        desc.debugName = "TrailSceneSnapshot";
        if (!m_gfx->CreateTexture(desc, s.tex)) return;
        s.width = w;
        s.height = h;
        s.state = RHI::ResourceState::COPY_DST;
    }
    s.ready = true;
}

bool TrailRenderPass::IsActive() const
{
    return !RG::PassActivityGatesEnabled()
        || (m_sys != nullptr && m_renderCB.IsValid()
            && m_sys->GetMaxActiveSlot() != 0 && !m_sys->GetDrawBuckets().empty());
}

RHI::CommandList TrailRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_renderCB.IsValid()) return cl;
    if (m_sys->GetMaxActiveSlot() == 0) return cl;   // nothing to draw
    const auto& buckets = m_sys->GetDrawBuckets();
    if (buckets.empty()) return cl;

    auto& gfx = *m_gfx;

    // Never sample the active RTV. The scoped copy restores HDR to RT without
    // mutating the shared state tracker while graph workers record in parallel.
    SceneSnapshot& scene = *m_scene;
    if (scene.ready)
    {
        if (scene.state != RHI::ResourceState::COPY_DST)
            gfx.PushBarrier(RHI::GPUBarrier::Image(&scene.tex, scene.state,
                RHI::ResourceState::COPY_DST), cl);
        gfx.CopyHdrSceneToScoped(scene.tex, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Image(&scene.tex,
            RHI::ResourceState::COPY_DST, RHI::ResourceState::SHADER_RESOURCE), cl);
        scene.state = RHI::ResourceState::SHADER_RESOURCE;
    }

    using namespace DirectX;

    // Per-frame instance-decode budget from CollectTrails (1 x 1 when every trail is
    // single-point). The CB write and the DrawInstanced count must derive from these
    // same locals: a decode desync scrambles which instance maps to which quad.
    const uint32_t frameMaxPc     = (std::max)(m_sys->GetFrameMaxPointCount(), 1u);
    const uint32_t frameMaxSubdiv = (std::max)(m_sys->GetFrameMaxSubdiv(), 1u);

    if (auto* slot = m_renderCB.Current(gfx))
    {
        RenderCB cb{};
        XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                        XMMatrixTranspose(vp));
        cb.camForward[0] = m_camForward.x;
        cb.camForward[1] = m_camForward.y;
        cb.camForward[2] = m_camForward.z;
        cb.maxSegments   = TrailSystem::kMaxSegmentsPerTrail;
        cb.maxPoints     = frameMaxPc;
        cb.maxSubdiv     = frameMaxSubdiv;
        *slot = cb;
    }

    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));

    cl.SetViewport();
    cl.SetScissorRect();

    // 6 verts per quad x (maxSegments-1) pairs x point strips x subdivisions, up to
    // the highest leased slot (slots past it have count==0; freed slots below it were
    // ResetOnly-cleared by the reconcile path).
    const uint32_t vertsPerInstance = 6;
    const uint32_t stripsPerPair = (std::max)(frameMaxPc, 2u) - 1u;   // mirrors the VS's P1
    // perTrail MUST equal the VS's `perTrail` (Trail.vs.hlsl instance decode).
    const uint32_t perTrail = (TrailSystem::kMaxSegmentsPerTrail - 1)
        * stripsPerPair * frameMaxSubdiv;
    const uint32_t instances = m_sys->GetMaxActiveSlot() * perTrail;

    const uint64_t texTable = gfx.GetBindlessTextureTableGpuHandle();

    uint32_t timeBits = 0;
    std::memcpy(&timeBits, &m_timeSec, sizeof(timeBits));

    // One draw per occupied (custom shader, TAA variant) bucket; the VS degenerates
    // trails whose header bucket differs from the b0 filters, so nothing double-draws.
    // Typical scenes have one bucket, so one draw.
    for (const TrailSystem::DrawBucket& b : buckets)
    {
        // Resolve this bucket's custom PS -> dynamic ID in this pass's library
        // (RegisterDynamic dedups by path; empty path -> engine default).
        uint32_t psID = 0;
        const std::string& path = m_sys->GetShaderBucketPath(b.shaderBucket);
        if (!path.empty())
        {
            const uint32_t id = m_shaderLib.RegisterDynamic(
                path.c_str(), RHI::ShaderStage::PS, "main");
            if (id != ShaderLibrary::kInvalidDynShaderID) psID = id;
        }

        const RHI::PipelineState* pso = m_psoCache.GetOrCreate(
            BuildTrailPSODesc(psID, b.taaVariant != 0));
        if (!pso || !pso->IsValid()) continue;   // broken custom shader -> skip this bucket only

        cl.SetPipelineState(*pso);
        cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
        if (b.taaVariant != 0)
            gfx.SetStencilRef(kTrailTAAExcludeBit, cl);

        // Re-bind all root args: SetPipelineState rebinds the root sig, which resets them
        // (engine convention, see ParticleRenderPass).
        gfx.SetGraphicsRootConstant(S::RootConstants, b.taaVariant,   /*offsetWords=*/0, cl); // g_variantFilter
        gfx.SetGraphicsRootConstant(S::RootConstants, b.shaderBucket, /*offsetWords=*/1, cl); // g_shaderBucket
        gfx.SetGraphicsRootConstant(S::RootConstants, timeBits,       /*offsetWords=*/2, cl); // g_timeBits
        gfx.BindConstantBuffer(SRel::CB_b2, m_renderCB.CurrentBuffer(gfx), cl);
        cl.BindDescriptorTableHandle(kGfxSegmentSRVSlot, m_sys->GetSegmentSRVHandle());
        cl.BindDescriptorTableHandle(kGfxHeaderSRVSlot,  m_sys->GetHeaderSRVHandle());
        // The HDR scene snapshot for custom trail PSs at t6 (S::SRV_t6), when
        // PrepareSceneSnapshot made one this frame.
        if (scene.ready)
            cl.BindDescriptorTableHandle(S::SRV_t6, gfx.GetTextureSRVGpuHandle(scene.tex));
        if (texTable)
            cl.BindDescriptorTableHandle(kGfxBindlessTexSlot, texTable);
        // b4: the fog the opaque scene got, and the soft-fade depth (TrailOutput(i, rgb, a)).
        FogApply::Bind(cl);

        if (b.shaderBucket == 0)
        {
            // Default bucket: one instanced draw covers every trail without a custom shader;
            // no slot filter, no b8 params.
            gfx.SetGraphicsRootConstant(S::RootConstants, 0xFFFFFFFFu, /*offsetWords=*/3, cl);
            gfx.DrawInstanced(vertsPerInstance, instances, 0, 0, cl);
        }
        else
        {
            // Custom bucket: one trail per draw, so each gets its own b8 param CBV
            // (Gfx::CustomMatCBV via BindCustomMaterialCBV, the root CBV the material system
            // uses). ready=false: the shader is still compiling this frame; skip rather than
            // let the PS read an unbound b8.
            // Issue only this trail's perTrail instances (N custom trails would otherwise cost
            // N^2 instances); the VS rebases SV_InstanceID by g_slotFilter * perTrail so the
            // decode still lands on this slot (Trail.vs.hlsl).
            for (const TrailSystem::SlotDraw& sd : b.slotDraws)
            {
                if (!sd.ready) continue;
                if (sd.paramVA)
                    gfx.BindCustomMaterialCBV(sd.paramVA, cl);
                gfx.SetGraphicsRootConstant(S::RootConstants, sd.slot, /*offsetWords=*/3, cl);
                gfx.DrawInstanced(vertsPerInstance, perTrail, 0, 0, cl);
            }
        }
    }

    return cl;
}
