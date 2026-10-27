#include "RenderGraph/RenderPass/HeightFogPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Composited ONE / INV_SRC_ALPHA over the HDR scene with the depth bound -
    // the froxel VolumetricFogPass apply's blend, a different PS.
    RG::FullscreenDraw ApplyDraw(RG::RGTextureHandle depth)
    {
        return { .ps      = ShaderID::HeightFogApply_PS,
                 .target  = RG::FullscreenTarget::HdrWithDepth(depth),
                 .blend   = RG::FullscreenBlend::PremultipliedOver,
                 .gpuZone = "HeightFog.Apply" };
    }
}

void HeightFogPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth declared as SRV: the graph emits DEPTHSTENCIL -> DEPTH_READ_SRV before
    // this pass. Do NOT declare HdrSceneColor as the colour target - the graph would
    // CLEAR it on entry even when the pass is disabled (as VolumetricFogPass /
    // SkyboxPass avoid too).
    b.ReadSRV(m_depth);
    // The DSV is bound with the depth test off (the fullscreen PSO's format match):
    // a plane the DSV does not use.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    // The HDR scene colour: declared - Phase A brings it to RENDER_TARGET, never
    // binds or clears it - and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void HeightFogPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS, RHI::ShaderStage::VS,
                         "VolumetricApply.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::HeightFogApply_PS, RHI::ShaderStage::PS,
                         "HeightFogApply.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);

    // Warmed for the scene depth's format; the draw takes it from the texture.
    if (!m_psoCache.GetOrCreate(RG::FullscreenPipelineDesc(ApplyDraw(m_depth), gfx.GetHdrSceneColorFormat(),
                                                           RHI::Format::D32_FLOAT_S8X24_UINT)))
        LOG_ERROR("HeightFogPass: apply PSO failed");
    // (HeightFogApply.ps samples with the fixed SMP_LINEAR_CLAMP heap slot, so the
    // pass creates no sampler.)

    LOG_SUCCESS("HeightFogPass: initialised");
}

HeightFogPass::HeightFogConstants HeightFogPass::BuildConstants() const
{
    HeightFogConstants c{};
    c.fogDensity            = m_params.fogDensity;
    c.fogHeightFalloff      = std::max(m_params.fogHeightFalloff, 0.0f);
    c.fogHeight             = m_params.fogHeight;
    c.startDistance         = std::max(std::max(m_params.startDistance, 0.0f), m_startFloor);
    c.fogColor[0]           = m_params.fogColor.x;
    c.fogColor[1]           = m_params.fogColor.y;
    c.fogColor[2]           = m_params.fogColor.z;
    c.maxOpacity            = std::clamp(m_params.maxOpacity, 0.0f, 1.0f);
    c.sunInscatterIntensity = std::max(m_params.sunInscatterIntensity, 0.0f);
    c.anisotropy            = std::clamp(m_params.anisotropy, 0.0f, 0.99f);
    return c;
}

bool HeightFogPass::ForwardConstants(HeightFogConstants& out) const
{
    // Execute's early-outs, plus the shader's own (FogDensity <= 0 returns no fog).
    if (!m_enabled || !Shown() || !m_gfx) return false;
    out = BuildConstants();
    return out.fogDensity > 0.0f;
}

RHI::CommandList HeightFogPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || !Shown()) return cl;
    if (!m_gfx) return cl;

    RG::DrawFullscreen(cl, m_psoCache, ApplyDraw(m_depth),
                       RHI::Slots::GfxRel::CB_b2, BuildConstants(),
                       { { RHI::Slots::GfxRel::CB_b1, RG::RGSlot::LightCB } },   // sun dir/color, cameraPos, invViewProj
                       { { RHI::Slots::GfxRel::SRV_t5, m_depth } });
    return cl;
}
