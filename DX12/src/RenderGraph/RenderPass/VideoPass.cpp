#include "RenderGraph/RenderPass/VideoPass.h"
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>

// Default-root-sig slots the composite PS references (RHI::Slots,
// Graphics/RootSignatureLayout.h). The IBL t6/t7 tables are repurposed here for the
// NV12 planes, hence the local names.
//   t6 space0 - Y plane SRV  -> absolute root slot Gfx::SRV_t6
//   t7 space0 - UV plane SRV -> absolute root slot Gfx::SRV_t7
//   b2 space0 - CB           -> BindConstantBuffer takes the group-relative index
static constexpr auto kYPlaneRootSlot   = RHI::Slots::Gfx::SRV_t6;
static constexpr auto kUVPlaneRootSlot  = RHI::Slots::Gfx::SRV_t7;
static constexpr auto kCompositeCBSlot  = RHI::Slots::GfxRel::CB_b2; // b2 (group-relative)

VideoPass::VideoPass() = default;

VideoPass::~VideoPass()
{
    if (m_gfx)
        m_cb.Destroy(*m_gfx);
}

void VideoPass::Setup(RG::RenderGraphBuilder& b)
{
    // Do not declare HdrSceneColor as the colour target: the graph would clear HDR on
    // entry, wiping Lighting / Skybox / Clouds before this blends on top. The HDR scene
    // colour is declared as a render target (Phase A brings it to RENDER_TARGET, never
    // binds or clears it) and bound in Execute, as CloudPass does.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void VideoPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Shaders ----
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VideoComposite_VS, RHI::ShaderStage::VS,
                         "VideoComposite.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::VideoComposite_PS, RHI::ShaderStage::PS,
                         "VideoComposite.ps.hlsl", "main");

    // ---- PSO cache ----
    m_psoCache.Init(gfx, m_shaderLib);
    {
        PSODesc desc;
        desc.vsID        = ShaderID::VideoComposite_VS;
        desc.psID        = ShaderID::VideoComposite_PS;
        desc.inputLayout = InputLayoutType::None;
        desc.rs.cullMode         = RHI::CullMode::NONE;
        desc.rs.depthClipEnable = false;
        desc.dss.depthEnable     = false;
        desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        // Standard SRC_ALPHA over blend: alpha=1 replaces the dst, alpha<1 lets the
        // under-content show through (UI / overlay use).
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
            LOG_ERROR("VideoPass: composite PSO create failed");
    }

    // ---- Constant buffer ----
    // The composite samples with the fixed SMP_LINEAR_CLAMP heap slot, so there is no
    // sampler to create.
    m_cb.Create(gfx, "Video.CompositeCB");

    LOG_SUCCESS("VideoPass: initialised");
}

void VideoPass::SetActiveFrame(const VideoComponent* vc)
{
    m_activeFrame = vc;
}

void VideoPass::SetRect(float uMin, float vMin, float uMax, float vMax)
{
    m_rectUV[0] = uMin; m_rectUV[1] = vMin;
    m_rectUV[2] = uMax; m_rectUV[3] = vMax;
}

// Keep in lockstep with Execute's early-outs. The Renderer pushes m_activeFrame
// before the graph runs, and the DPB contents it points at are settled by then.
bool VideoPass::IsActive() const
{
    if (!m_gfx || !m_psoCreated) return false;
    if (!m_activeFrame)          return false;
    if (m_activeFrame->currentDpbSlot >= m_activeFrame->dpb.size()) return false;
    return m_activeFrame->dpb[m_activeFrame->currentDpbSlot].IsValid();
}

RHI::CommandList VideoPass::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_psoCreated)                       return cl;
    if (!m_activeFrame)                                return cl;
    if (m_activeFrame->currentDpbSlot >= m_activeFrame->dpb.size()) return cl;

    const RHI::Texture& nv12 =
        m_activeFrame->dpb[m_activeFrame->currentDpbSlot];
    if (!nv12.IsValid())                               return cl;

    IGraphicsDevice& gfx = *m_gfx;

    // ---- Upload CB ----
    if (auto* slot = m_cb.Current(gfx))
    {
        VideoCompositeCB c{};
        std::memcpy(c.rectUV, m_rectUV, sizeof(c.rectUV));
        c.alpha      = m_alpha;
        c.colorSpace = m_colorSpace;
        *slot = c;
    }

    // ---- Bind HDR colour as RT (no clear) ----
    // No depth: the composite has depth test off, and SetRenderTargetToHdrWithDepth
    // accepts a null DSV.
    gfx.SetRenderTargetToHdrWithDepth(nullptr, cl);

    cl.BindDescriptorHeaps();

    const PSODesc desc = [this]{
        PSODesc d;
        d.vsID        = ShaderID::VideoComposite_VS;
        d.psID        = ShaderID::VideoComposite_PS;
        d.inputLayout = InputLayoutType::None;
        d.rs.cullMode         = RHI::CullMode::NONE;
        d.rs.depthClipEnable = false;
        d.dss.depthEnable     = false;
        d.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
        auto& bs = d.bs.renderTarget[0];
        bs.blendEnable             = true;
        bs.srcBlend                = RHI::Blend::SRC_ALPHA;
        bs.destBlend               = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOp                 = RHI::BlendOp::ADD;
        bs.srcBlendAlpha          = RHI::Blend::ONE;
        bs.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOpAlpha           = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
        d.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtvCount      = 1;
        d.dsvFormat     = RHI::Format::D32_FLOAT_S8X24_UINT;
        return d;
    }();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
    if (!pso || !pso->IsValid()) return cl;

    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);

    // CB -> b2 space0
    gfx.BindConstantBuffer(kCompositeCBSlot, m_cb.CurrentBuffer(gfx), cl);

    // Y plane -> t6 space0 (root slot Gfx::SRV_t6)
    cl.BindDescriptorTableHandle(kYPlaneRootSlot,
        gfx.GetTextureSRVGpuHandle(nv12));
    // UV plane -> t7 space0 (root slot Gfx::SRV_t7)
    cl.BindDescriptorTableHandle(kUVPlaneRootSlot,
        gfx.GetTextureUVPlaneSRVGpuHandle(nv12));

    cl.DrawInstanced(3, 1, 0, 0);
    return cl;
}
