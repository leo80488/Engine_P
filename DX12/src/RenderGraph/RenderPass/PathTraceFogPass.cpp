#include "RenderGraph/RenderPass/PathTraceFogPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

namespace
{
    // Composited ONE / INV_SRC_ALPHA over the HDR scene, as the height fog is.
    RG::FullscreenDraw ApplyDraw(RG::RGTextureHandle depth)
    {
        return { .ps      = ShaderID::PathTraceFogApply_PS,
                 .target  = RG::FullscreenTarget::HdrWithDepth(depth),
                 .blend   = RG::FullscreenBlend::PremultipliedOver,
                 .gpuZone = "PathTraceFog.Apply" };
    }
}

void PathTraceFogPass::Setup(RG::RenderGraphBuilder& b)
{
    // HeightFogPass's declarations: the depth read-only (the DSV the fullscreen pipeline's
    // formats bind), the HDR scene loaded and bound in Execute, never cleared.
    b.ReadSRV(m_depth);
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void PathTraceFogPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS, RHI::ShaderStage::VS, "VolumetricApply.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::PathTraceFogApply_PS, RHI::ShaderStage::PS, "PathTraceFogApply.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);
    // Warmed for the scene depth's format; the draw takes it from the texture.
    if (!m_psoCache.GetOrCreate(RG::FullscreenPipelineDesc(ApplyDraw(m_depth), gfx.GetHdrSceneColorFormat(),
                                                           RHI::Format::D32_FLOAT_S8X24_UINT)))
        LOG_ERROR("PathTraceFogPass: apply PSO failed");
}

RHI::CommandList PathTraceFogPass::Execute(RHI::CommandList cl)
{
    if (!m_fogIdx || !m_gfx) return cl;
    Constants c{};
    c.fogIdx = m_fogIdx;
    RG::DrawFullscreen(cl, m_psoCache, ApplyDraw(m_depth), RHI::Slots::GfxRel::CB_b2, c);
    return cl;
}
