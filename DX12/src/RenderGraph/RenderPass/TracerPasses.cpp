#include "RenderGraph/RenderPass/TracerPasses.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/TracerSystem.h"
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

// Compute root signature slots (RHI::Slots::Cmp), the same physical slots
// ParticleSimPass uses; only the t0 SRV differs (gSpawns instead of restPositions).
static constexpr auto kCSCBSlot       = C::CB;      // b0 space2 (ROOT_CBV)
static constexpr auto kCSSpawnsSlot   = C::SRV_t0;  // DESC_TABLE t0 space2 (spawn upload SRV)
static constexpr auto kCSPoolUAVSlot  = C::UAV_u0;  // DESC_TABLE u0 space2 (pool UAV)

// Graphics root signature slots (see RHI::Slots::Gfx).
static constexpr auto kGfxPoolSRVSlot    = S::SRV_t4;      // DESC_TABLE t4 space0
static constexpr auto kGfxDepthSRVSlot   = S::SRV_t5;      // DESC_TABLE t5 space0
static constexpr auto kGfxBindlessTexSlot= S::BindlessTex; // DESC_TABLE t0 space2 - bindless texture array

// ==== TracerSimPass ====

void TracerSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TracerEmit_CS,   RHI::ShaderStage::CS, "TracerEmit.cs.hlsl",   "CSMain");
    m_shaderLib.Register(ShaderID::TracerUpdate_CS, RHI::ShaderStage::CS, "TracerUpdate.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("TracerSimPass: initialised");
}

// Creates into the live members; a failed rebuild keeps the old pipeline.
bool TracerSimPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* emitCS   = m_shaderLib.GetShader(ShaderID::TracerEmit_CS);
    const RHI::Shader* updateCS = m_shaderLib.GetShader(ShaderID::TracerUpdate_CS);
    if (!emitCS || !updateCS)
    {
        LOG_ERROR("TracerSimPass: compute shader lookup failed");
        return false;
    }

    {
        RHI::PipelineStateDesc d{};
        d.cs = emitCS;
        if (!gfx.CreatePipelineState(d, m_emitPSO))
            LOG_ERROR("TracerSimPass: emit PSO failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = updateCS;
        if (!gfx.CreatePipelineState(d, m_updatePSO))
            LOG_ERROR("TracerSimPass: update PSO failed");
    }
    return true;
}

void TracerSimPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void TracerSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_emitPSO.IsValid() || !m_updatePSO.IsValid()) return;
    auto& gfx = *m_gfx;

    // ---- Phase A: Emit (single dispatch over all spawns this frame) ----
    const uint32_t spawnCount = m_sys->GetSpawnCount();
    if (spawnCount > 0)
    {
        gfx.BindComputePipelineState(m_emitPSO, cl);
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(gfx), 0, cl);
        gfx.SetComputeDescriptorTable(kCSSpawnsSlot,  m_sys->GetSpawnSRV(gfx), cl);
        gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAV(), cl);

        gfx.DispatchCompute((spawnCount + 63) / 64, 1, 1, cl);

        // Emit writes must complete before update reads.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPool()), cl);
    }

    // ---- Phase B: Update (one dispatch over the entire pool) ----
    gfx.BindComputePipelineState(m_updatePSO, cl);
    gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetSystemCB(gfx), 0, cl);
    gfx.SetComputeDescriptorTable(kCSPoolUAVSlot, m_sys->GetPoolUAV(), cl);

    const uint32_t poolSize = TracerSystem::GetPoolCapacity();
    gfx.DispatchCompute((poolSize + 63) / 64, 1, 1, cl);

    // Render pass needs the pool in a stable state.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPool()), cl);
}

// ==== TracerRenderPass ====

TracerRenderPass::TracerRenderPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{}

void TracerRenderPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth is sampled in the PS for the soft-particle fade; no DSV is bound. The HDR
    // RTV is bound in Execute, as in TransparentPass. With planes on, depth is a plain
    // SRV read and the graph transitions it.
    // With -rgplaneoff: ManualBarrier with a declared DEPTHSTENCIL entry state (Phase A
    // brings depth there whatever the last depth pass left) and Execute's
    // DEPTHSTENCIL -> SHADER_RESOURCE -> DEPTHSTENCIL round trip; do not add a ReadSRV
    // beside it, it would double-transition.
    if (RG::RenderGraph::PlanesEnabled()) b.ReadSRV(m_depth);
    else                                  b.ManualBarrier(m_depth, RHI::ResourceState::DEPTHSTENCIL);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds
    // or clears it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The froxel fog FogApply samples (VolumetricFogPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kVolFogScattering });
}

void TracerRenderPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Tracer_VS, RHI::ShaderStage::VS, "Tracer.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::Tracer_PS, RHI::ShaderStage::PS, "Tracer.ps.hlsl", "main");

    if (!CreatePipelines(gfx)) return;

    // Per-frame render CB.
    if (!m_renderCB.Create(gfx, "TracerRenderPass.RenderCB"))
        LOG_ERROR("TracerRenderPass: render CB create failed");
    // Tracer.ps samples the optional noise texture with the fixed SMP_LINEAR_WRAP heap
    // slot and its fog with SMP_LINEAR_CLAMP, so the pass creates no sampler.

    LOG_SUCCESS("TracerRenderPass: initialised");
}

// Creates into the live member; a failed rebuild keeps the old pipeline.
bool TracerRenderPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Tracer_VS);
    const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::Tracer_PS);
    if (!vs || !ps)
    {
        LOG_ERROR("TracerRenderPass: shader lookup failed");
        return false;
    }

    // PSO: triangle strip, 4 verts/instance, additive blend (SRC_ALPHA, ONE), no depth
    // test or write (the PS samples depth via SRV).
    RHI::RasterizerState rs{};
    rs.fillMode         = RHI::FillMode::SOLID;
    rs.cullMode         = RHI::CullMode::NONE;       // double-sided
    rs.depthClipEnable = true;

    RHI::DepthStencilState dss{};
    dss.depthEnable     = false;                     // depth handled in PS
    dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    dss.stencilEnable   = false;

    RHI::BlendState bs{};
    auto& rt = bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.destBlend               = RHI::Blend::ONE;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    rt.destBlendAlpha         = RHI::Blend::ONE;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    RHI::PipelineStateDesc d{};
    d.vs             = vs;
    d.ps             = ps;
    d.rs             = &rs;
    d.dss            = &dss;
    d.bs             = &bs;
    d.pt             = RHI::PrimitiveTopology::TRIANGLESTRIP;
    d.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    d.rtvCount      = 1;
    d.dsvFormat     = RHI::Format::UNKNOWN;          // no DSV
    d.sampleCount   = 1;

    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("TracerRenderPass: PSO creation failed");
        return false;
    }
    return true;
}

void TracerRenderPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

bool TracerRenderPass::IsActive() const
{
    return !RG::PassActivityGatesEnabled()
        || (m_sys != nullptr && m_pso.IsValid() && m_renderCB.IsValid()
            && m_sys->HasLiveTracers());
}

RHI::CommandList TracerRenderPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_pso.IsValid() || !m_renderCB.IsValid()) return cl;

    // Pool provably empty: skip the pass instead of transitioning depth twice and
    // drawing every pool slot as a degenerate instance. Keep this before both depth
    // barriers: the round trip is self-contained, so returning here hands depth back in
    // the state the graph gave. Nothing before this has a side effect the frame depends
    // on (the render CB is read only by this draw and rewritten on the next one).
    if (!m_sys->HasLiveTracers()) return cl;

    auto& gfx = *m_gfx;

    using namespace DirectX;

    // Pack the render CB. Matrix transposed for HLSL row-vector mul().
    if (auto* slot = m_renderCB.Current(gfx))
    {
        RenderCB cb{};
        XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.viewProj),
                        XMMatrixTranspose(vp));
        cb.cameraPos[0]  = m_cameraPos.x;
        cb.cameraPos[1]  = m_cameraPos.y;
        cb.cameraPos[2]  = m_cameraPos.z;
        cb.time          = m_time;
        cb.nearZ         = m_nearZ;
        cb.farZ          = m_farZ;
        cb.fadeRange     = m_fadeRange;
        cb.coreSharpness = m_coreSharpness;
        cb.noiseTiling   = m_noiseTiling;
        cb.scrollSpeed   = m_scrollSpeed;
        cb.noiseFloor    = m_noiseFloor;
        *slot = cb;
    }

    // With -rgplaneoff, transition depth DEPTHSTENCIL -> SHADER_RESOURCE here so the
    // PS can sample it, and restore it after the draw.
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    if (!depthTex) return cl;

    const bool planes = RG::RenderGraph::PlanesEnabled();   // true: the graph already transitioned depth
    if (!planes)
        cl.PushBarrier(RHI::GPUBarrier::Image(
            depthTex,
            RHI::ResourceState::DEPTHSTENCIL,
            RHI::ResourceState::SHADER_RESOURCE));

    // Bind HDR RTV with NO DSV - the PS does the depth compare.
    gfx.SetRenderTargetToHdrWithDepth(nullptr, cl);
    cl.SetViewport();
    cl.SetScissorRect();

    cl.SetPipelineState(m_pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLESTRIP);

    // Render CB at b2 space0.
    gfx.BindConstantBuffer(SRel::CB_b2, m_renderCB.CurrentBuffer(gfx), cl);

    // Pool SRV at t4 space0.
    cl.BindDescriptorTableHandle(kGfxPoolSRVSlot, m_sys->GetPoolSRV());

    // Scene depth SRV at t5 space0.
    const uint64_t depthSrv = gfx.GetTextureSRVGpuHandle(*depthTex);
    if (depthSrv)
        cl.BindDescriptorTableHandle(kGfxDepthSRVSlot, depthSrv);

    // Bindless texture table: Tracer.ps.hlsl indexes g_AllTextures[noiseIdx] when a
    // tracer carries a non-sentinel noise index.
    const uint64_t texTable = gfx.GetBindlessTextureTableGpuHandle();
    if (texTable)
        cl.BindDescriptorTableHandle(kGfxBindlessTexSlot, texTable);

    // b4: the fog the opaque scene got (applied in Tracer.ps).
    FogApply::Bind(cl);

    // 4 verts (TRIANGLESTRIP) per instance, one instance per pool slot.
    // Dead slots emit a degenerate quad in VS so the rasteriser skips them.
    gfx.DrawInstanced(4, TracerSystem::GetPoolCapacity(), 0, 0, cl);

    // Restore depth to DEPTHSTENCIL (-rgplaneoff): the graph's tracker records this
    // pass as leaving it there (ManualBarrier's entry state), and the next pass's
    // transition starts from it.
    if (!planes)
        cl.PushBarrier(RHI::GPUBarrier::Image(
            depthTex,
            RHI::ResourceState::SHADER_RESOURCE,
            RHI::ResourceState::DEPTHSTENCIL));

    return cl;
}
