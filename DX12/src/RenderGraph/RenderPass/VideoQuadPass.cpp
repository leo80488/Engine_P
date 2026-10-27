#include "RenderGraph/RenderPass/VideoQuadPass.h"
#include "ECS/ECS.h"
#include "ECS/VideoComponent.h"
#include "ECS/HierarchyComponents.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassTextureNames.h"
#include "System/Log.h"
#include "System/ThreadContext.h"

#include <algorithm>
#include <cstring>

using namespace DirectX;

// Same root-sig slots as the screen-space VideoPass (RHI::Slots,
// Graphics/RootSignatureLayout.h). The IBL t6/t7 tables are repurposed for the
// NV12 planes, hence the local names.
//   t6 space0 -> absolute root slot Gfx::SRV_t6 (Y plane)
//   t7 space0 -> absolute root slot Gfx::SRV_t7 (UV plane)
//   b2 space0 -> BindConstantBufferAtOffset takes the group-relative index
static constexpr auto kYPlaneRootSlot  = RHI::Slots::Gfx::SRV_t6;
static constexpr auto kUVPlaneRootSlot = RHI::Slots::Gfx::SRV_t7;
static constexpr auto kCBSlot          = RHI::Slots::GfxRel::CB_b2; // b2 (group-relative)

VideoQuadPass::VideoQuadPass(RG::RGTextureHandle depth)
    : m_depth(depth)
{
    XMStoreFloat4x4(&m_viewProj, XMMatrixIdentity());
}

VideoQuadPass::~VideoQuadPass()
{
    if (m_gfx)
    {
        for (auto& ring : m_cbRing)
        {
            if (ring.mapped) m_gfx->UnmapBuffer(ring.buffer);
            if (ring.buffer.IsValid()) m_gfx->DestroyBuffer(ring.buffer);
            ring.mapped = nullptr;
            ring.capacitySlots = 0;
        }
    }
}

void VideoQuadPass::Setup(RG::RenderGraphBuilder& b)
{
    // Planes on: depth is only tested, never sampled - a DSV read. -rgplaneoff: an
    // SRV read.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::Read);
    else                                  b.ReadSRV(m_depth);
    // The HDR scene colour: declared (Phase A brings it to RENDER_TARGET, never binds
    // or clears it) and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
    // The froxel fog FogApply samples (VolumetricFogPass barriers it).
    b.ReadPassTextures({ RG::PassTex::kVolFogScattering });
}

void VideoQuadPass::EnsureCBRing(IGraphicsDevice& gfx, uint32_t frameIdx, uint32_t slots)
{
    if (frameIdx >= kFrameCount) return;
    auto& ring = m_cbRing[frameIdx];
    if (ring.buffer.IsValid() && ring.capacitySlots >= slots) return;

    // Free the old one (deferred-release).
    if (ring.mapped) { gfx.UnmapBuffer(ring.buffer); ring.mapped = nullptr; }
    if (ring.buffer.IsValid()) gfx.DestroyBuffer(ring.buffer);

    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(slots) * RHI::kMaxViews * kCBStride;
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
    if (!gfx.CreateBuffer(bd, ring.buffer))
    {
        LOG_ERROR("VideoQuadPass: CB ring buffer create failed (slots=%u)", slots);
        ring.capacitySlots = 0;
        return;
    }
    ring.mapped = static_cast<uint8_t*>(gfx.MapBuffer(ring.buffer));
    ring.capacitySlots = slots;
    LOG_INFO("VideoQuadPass: CB ring slot %u sized for %u quads a view (%llu B)",
             frameIdx, slots, bd.size);
}

void VideoQuadPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VideoQuad_VS, RHI::ShaderStage::VS,
                         "VideoQuad.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VideoQuad_PS, RHI::ShaderStage::PS,
                         "VideoQuad.ps.hlsl", "main");

    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VideoQuad_VS;
        desc.psID        = ShaderID::VideoQuad_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode         = RHI::CullMode::NONE;
        desc.rs.depthClipEnable = true;
        desc.dss.depthEnable     = true;
        desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;
        auto& bs = desc.bs.renderTarget[0];
        bs.blendEnable             = true;
        bs.srcBlend                = RHI::Blend::SRC_ALPHA;
        bs.destBlend               = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOp                 = RHI::BlendOp::ADD;
        bs.srcBlendAlpha          = RHI::Blend::ONE;
        bs.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOpAlpha           = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        desc.rtvCount      = 1;
        desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

        if (m_psoCache.GetOrCreate(desc))
            m_psoCreated = true;
        else
            LOG_ERROR("VideoQuadPass: PSO create failed");
    }

    // Initial CB rings: 16 slots x 256 B = 4 KB per frame buffer.
    for (uint32_t i = 0; i < kFrameCount; ++i)
        EnsureCBRing(gfx, i, kCBInitialSlots);
    // VideoQuad.ps and its fog sample with the fixed SMP_LINEAR_CLAMP heap slot, so
    // there is no sampler to create.

    LOG_SUCCESS("VideoQuadPass: initialised");
}

void VideoQuadPass::SetCamera(const XMFLOAT4X4& viewProjNoJitter)
{
    m_viewProj = viewProjNoJitter;
}

void VideoQuadPass::SetViewCamera(const XMFLOAT4X4& viewProjNoJitter)
{
    if (m_boundSlot != 0) *m_cam = viewProjNoJitter;
}

void VideoQuadPass::BindView(ViewState& view)
{
    m_boundSlot = view.Slot();
    m_cam       = m_boundSlot == 0 ? &m_viewProj : &view.Data<ViewCamera>(this).viewProj;
}

RHI::CommandList VideoQuadPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_psoCreated) return cl;

    IGraphicsDevice& gfx = *m_gfx;
    const uint32_t   frameIdx = gfx.GetFrameIndex() % kFrameCount;

    // Honour any growth request from the previous frame BEFORE we start
    // writing into this slot. The deferred-release of the old buffer is safe
    // because GraphicsDX12 holds it until its frame-slot fence retires.
    if (m_pendingGrowSlots > 0)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
            EnsureCBRing(gfx, i, m_pendingGrowSlots);
        m_pendingGrowSlots = 0;
    }

    auto& ring = m_cbRing[frameIdx];
    if (!ring.mapped || ring.capacitySlots == 0) return cl;

    // Execute reads only the extracted quads (see the Quad lane in the header), never
    // the World: it runs on a RenderGraph Phase-B TaskSystem worker.
    if (m_quads.empty()) return cl;

    // If this batch exceeds capacity, schedule a grow for next frame and drop the
    // rest this frame (with a warning), so the mapped buffer is never overrun.
    size_t drawCount = m_quads.size();
    if (drawCount > ring.capacitySlots)
    {
        m_pendingGrowSlots = (std::max)(uint32_t(drawCount),
                                        ring.capacitySlots * 2);
        LOG_WARNING("VideoQuadPass: %zu world-space videos but ring holds %u "
                    "— dropping %zu this frame, growing to %u next frame",
                    drawCount, ring.capacitySlots,
                    drawCount - ring.capacitySlots, m_pendingGrowSlots);
        drawCount = ring.capacitySlots;
    }

    // ---- Bind common state ----
    const RHI::Texture* depthTex = cl.GetContext().GetTexture(m_depth);
    gfx.SetRenderTargetToHdrWithDepth(depthTex, cl, cl.GetContext().DepthView(m_depth));
    cl.BindDescriptorHeaps();

    PSODesc desc;
    desc.vsID        = ShaderID::VideoQuad_VS;
    desc.psID        = ShaderID::VideoQuad_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = true;
    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;
    auto& bs = desc.bs.renderTarget[0];
    bs.blendEnable             = true;
    bs.srcBlend                = RHI::Blend::SRC_ALPHA;
    bs.destBlend               = RHI::Blend::INV_SRC_ALPHA;
    bs.blendOp                 = RHI::BlendOp::ADD;
    bs.srcBlendAlpha          = RHI::Blend::ONE;
    bs.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
    bs.blendOpAlpha           = RHI::BlendOp::ADD;
    bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    desc.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;

    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return cl;

    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
    // b4: the fog the opaque scene got (applied in VideoQuad.ps).
    FogApply::Bind(cl);

    // ---- Per-quad: write CB into the bound view's region of the ring, bind, draw ----
    const uint64_t regionBase = uint64_t(m_boundSlot) * ring.capacitySlots;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const Quad& q = m_quads[i];

        // Write CB into ring at offset (region + i) * kCBStride.
        VideoQuadCB c{};
        std::memcpy(c.worldMatrix,    &q.world, sizeof(c.worldMatrix));
        std::memcpy(c.viewProjMatrix, m_cam,    sizeof(c.viewProjMatrix));
        c.quadWidth  = q.width;
        c.quadHeight = q.height;
        c.alpha      = q.alpha;
        c.colorSpace = q.colorSpace;
        std::memcpy(ring.mapped + (regionBase + i) * kCBStride, &c, sizeof(c));

        gfx.BindConstantBufferAtOffset(kCBSlot, ring.buffer,
                                       (regionBase + i) * kCBStride, cl);
        cl.BindDescriptorTableHandle(kYPlaneRootSlot,
            gfx.GetTextureSRVGpuHandle(q.nv12));
        cl.BindDescriptorTableHandle(kUVPlaneRootSlot,
            gfx.GetTextureUVPlaneSRVGpuHandle(q.nv12));
        cl.DrawInstanced(6, 1, 0, 0);
    }

    return cl;
}

// Capacity of the ring the next Execute will use (see the header).
uint32_t VideoQuadPass::QuadCapacity() const
{
    if (!m_gfx) return 0;
    return m_cbRing[m_gfx->GetFrameIndex() % kFrameCount].capacitySlots;
}
