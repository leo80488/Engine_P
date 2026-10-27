#include "RenderGraph/PassHelpers.h"
#include "RenderGraph/RenderContext.h"

namespace RG
{
PSODesc FullscreenPipelineDesc(const FullscreenDraw& draw, RHI::Format colorFormat,
                               RHI::Format depthFormat)
{
    PSODesc desc;
    desc.vsID        = draw.vs;
    desc.psID        = draw.ps;
    desc.inputLayout = InputLayoutType::None;
    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;
    if (draw.blend == FullscreenBlend::PremultipliedOver)
    {
        auto& bs = desc.bs.renderTarget[0];
        bs.blendEnable     = true;
        bs.srcBlend        = RHI::Blend::ONE;
        bs.destBlend       = RHI::Blend::INV_SRC_ALPHA;
        bs.blendOp         = RHI::BlendOp::ADD;
        bs.srcBlendAlpha  = RHI::Blend::ZERO;
        bs.destBlendAlpha = RHI::Blend::ONE;
        bs.blendOpAlpha   = RHI::BlendOp::ADD;
        bs.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;
    }
    desc.rtvFormats[0] = colorFormat;
    desc.rtvCount      = 1;
    desc.dsvFormat     = depthFormat;
    return desc;
}

namespace detail
{
bool DrawFullscreen(RHI::CommandList cl, PSOCache& cache, const FullscreenDraw& draw,
                    RHI::Slots::GfxCB constantsSlot, uint64_t constantsVA,
                    std::initializer_list<GraphCB> cbs, std::initializer_list<GraphSRV> srvs)
{
    IGraphicsDevice& gfx = cl.GetDevice();
    const RHI::Texture* depth = cl.GetContext().GetTexture(draw.target.depth);
    if (!depth) return false;

    // The pipeline for exactly what is about to be bound: the HDR target's format
    // and the depth texture's own (its RHI format is the DSV format; the backend
    // makes the resource typeless underneath).
    const RHI::PipelineState* pso = cache.GetOrCreate(
        FullscreenPipelineDesc(draw, gfx.GetHdrSceneColorFormat(), depth->desc.format));
    if (!pso || !pso->IsValid()) return false;

    // The DSV as the pass declared it (RenderContext::DepthView).
    gfx.SetRenderTargetToHdrWithDepth(depth, cl, cl.GetContext().DepthView(draw.target.depth));
    cl.BindDescriptorHeaps();
    cl.SetPipelineState(*pso);
    cl.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST);
    for (const GraphCB& c : cbs)
        cl.BindCB(c.slot, c.cb);
    cl.BindCBAddress(constantsSlot, constantsVA);
    for (const GraphSRV& s : srvs)
        cl.BindSRVByHandle(s.slot, s.texture);

    if (draw.gpuZone)
    {
        RG_GPU_SCOPE(cl, draw.gpuZone);
        cl.DrawInstanced(3, 1, 0, 0);
    }
    else
    {
        cl.DrawInstanced(3, 1, 0, 0);
    }
    return true;
}
} // namespace detail
} // namespace RG
