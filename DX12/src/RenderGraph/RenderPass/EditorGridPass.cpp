#include "RenderGraph/RenderPass/EditorGridPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

#include <algorithm>

namespace
{
    // Composited ONE / INV_SRC_ALPHA over the HDR scene with the depth bound -
    // the HeightFogPass apply's draw, a different PS.
    RG::FullscreenDraw GridDraw(RG::RGTextureHandle depth)
    {
        return { .ps      = ShaderID::EditorGrid_PS,
                 .target  = RG::FullscreenTarget::HdrWithDepth(depth),
                 .blend   = RG::FullscreenBlend::PremultipliedOver,
                 .gpuZone = "EditorGrid" };
    }
}

void EditorGridPass::Setup(RG::RenderGraphBuilder& b)
{
    // Depth declared as SRV -> the graph emits the DEPTHSTENCIL -> DEPTH_READ_SRV
    // barrier before this pass. Do NOT make HdrSceneColor the SetColorTarget: the graph
    // would CLEAR it on entry even when the pass is disabled (as in HeightFogPass /
    // VolumetricFogPass).
    b.ReadSRV(m_depth);
    // The DSV is bound with the depth test off (the fullscreen PSO's format match): a
    // plane the DSV does not use.
    if (RG::RenderGraph::PlanesEnabled()) b.DepthStencil(m_depth, RG::Depth::None);
    // The HDR scene colour: declared - Phase A brings it to RENDER_TARGET, never binds
    // or clears it - and bound in Execute.
    b.RenderTarget(b.HdrScene(), RG::Load::Load);
    b.SetColorTarget(RG::BuiltinTexture::None);
}

void EditorGridPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VolumetricApply_VS, RHI::ShaderStage::VS,
                         "VolumetricApply.vs.hlsl", "main");
    m_shaderLib.Register(ShaderID::EditorGrid_PS, RHI::ShaderStage::PS,
                         "EditorGrid.ps.hlsl", "main");
    m_psoCache.Init(gfx, m_shaderLib);

    // Warmed for the scene depth's format; the draw takes it from the texture.
    if (!m_psoCache.GetOrCreate(RG::FullscreenPipelineDesc(GridDraw(m_depth), gfx.GetHdrSceneColorFormat(),
                                                           RHI::Format::D32_FLOAT_S8X24_UINT)))
        LOG_ERROR("EditorGridPass: grid PSO failed");

    LOG_SUCCESS("EditorGridPass: initialised");
}

// Keep in lockstep with Execute's early-out. m_enabled is pushed by the editor
// before the graph runs; m_gfx is cached in Init.
bool EditorGridPass::IsActive() const
{
    return m_enabled && m_gfx != nullptr;
}

RHI::CommandList EditorGridPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled || !m_gfx) return cl;

    GridConstants c{};
    {
        const float cell = std::max(m_cellSize, 0.001f);
        c.cellSize     = cell;
        c.majorEvery   = 10.0f;
        c.gridY        = 0.0f;
        // Bigger units stay readable farther out before the radial fade.
        c.fadeDistance = std::max(500.0f, cell * 400.0f);
        const float minor[4] = { 0.52f, 0.52f, 0.52f, 0.32f };
        const float major[4] = { 0.65f, 0.65f, 0.65f, 0.52f };
        const float axisX[4] = { 0.90f, 0.28f, 0.28f, 0.90f };
        const float axisZ[4] = { 0.28f, 0.47f, 0.95f, 0.90f };
        std::copy(minor, minor + 4, c.minorColor);
        std::copy(major, major + 4, c.majorColor);
        std::copy(axisX, axisX + 4, c.axisXColor);
        std::copy(axisZ, axisZ + 4, c.axisZColor);
    }

    RG::DrawFullscreen(cl, m_psoCache, GridDraw(m_depth), RHI::Slots::GfxRel::CB_b2, c,
                       { { RHI::Slots::GfxRel::CB_b1, RG::RGSlot::LightCB } },   // cameraPos, invViewProj
                       { { RHI::Slots::GfxRel::SRV_t5, m_depth } });
    return cl;
}
