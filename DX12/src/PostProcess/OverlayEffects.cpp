#include "PostProcess/OverlayEffects.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ISurface.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/SceneView.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "Renderer/FrameResources.h"
#include "Renderer/FrameView.h"

namespace PostProcess
{

namespace
{
    // A GPU region around one pass's Execute (RAII), under the pass's own name.
    struct Region
    {
        Context& ctx;
        uint32_t id;
        Region(Context& c, const char* name) : ctx(c), id(c.gfx->BeginGPUTimestamp(c.cl, name)) {}
        ~Region() { ctx.gfx->EndGPUTimestamp(ctx.cl, id); }
    };
}

// ==== DebugWire ====
bool DebugWireEffect::IsEnabled(const Context& ctx) const
{
    return m_pass && m_pass->enabled && m_pass->HasLines() && m_toneMap && ctx.view &&
           (ctx.show & static_cast<uint32_t>(RG::ShowFlag::EditorOverlays));
}

void DebugWireEffect::Execute(Context& ctx)
{
    // Borrow depth as a PIXEL-shader SRV for the manual occlusion test (the
    // target is display-res and depth render-res, so no DSV can be paired
    // with it - see DebugWireDepth.ps.hlsl). The Renderer takes it back to
    // DEPTHSTENCIL after the stage.
    uint64_t wireDepthSrv = 0;
    if (ctx.depthTex && ctx.depthSrv && ctx.graph && ctx.textures)
    {
        const RHI::ResourceState cur = ctx.graph->GetTextureState(ctx.textures->depth);
        // The depth plane alone, and only when a pixel shader cannot read it as it is
        // (SHADER_RESOURCE and DEPTH_READ_SRV can). -rgplaneoff: the whole resource to
        // SHADER_RESOURCE.
        if (RG::RenderGraph::PlanesEnabled())
        {
            if (cur != RHI::ResourceState::SHADER_RESOURCE && cur != RHI::ResourceState::DEPTH_READ_SRV)
                ctx.graph->MovePlanes(*ctx.gfx, ctx.cl, ctx.textures->depth, RHI::ResourceState::DEPTH_READ_SRV,
                                      RHI::ResourceState::UNDEFINED);
        }
        else if (cur != RHI::ResourceState::SHADER_RESOURCE)
        {
            ctx.gfx->PushBarrier(RHI::GPUBarrier::Image(
                ctx.depthTex, cur, RHI::ResourceState::SHADER_RESOURCE), ctx.cl);
            ctx.graph->SetTextureState(ctx.textures->depth, RHI::ResourceState::SHADER_RESOURCE);
        }
        wireDepthSrv = ctx.depthSrv;
    }

    const FrameView& fv = *ctx.view;
    const RHI::ResourceState entry = m_toneMap->GetFinalOutputState();
    {
        Region r(ctx, "DebugWire");
        m_pass->Execute(ctx.cl, fv.view.viewProjMatrixNoJitter, m_toneMap->GetFinalOutputTexture(), entry,
                        fv.vpWidth, fv.vpHeight, wireDepthSrv, fv.renderWidth, fv.renderHeight);
    }
    m_toneMap->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
}

// ==== WorldUI ====
bool WorldUIEffect::IsEnabled(const Context& ctx) const
{
    return m_pass && m_pass->enabled && m_toneMap && ctx.view;
}

void WorldUIEffect::Execute(Context& ctx)
{
    // No World: the vertex lane was built on the game side by
    // Renderer::ExtractWorldUI, right after the UI tick that produces the
    // per-frame state it reads.
    const RHI::ResourceState entry = m_toneMap->GetFinalOutputState();
    {
        Region r(ctx, "WorldUI");
        m_pass->Execute(ctx.cl, m_toneMap->GetFinalOutputTexture(), entry, ctx.view->vpWidth, ctx.view->vpHeight);
    }
    m_toneMap->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
}

// ==== DebugIcons ====
bool DebugIconsEffect::IsEnabled(const Context& ctx) const
{
    return m_pass && m_pass->enabled && m_pass->HasIcons() && m_toneMap && ctx.view &&
           (ctx.show & static_cast<uint32_t>(RG::ShowFlag::EditorOverlays));
}

void DebugIconsEffect::Execute(Context& ctx)
{
    const FrameView& fv = *ctx.view;
    const RHI::ResourceState entry = m_toneMap->GetFinalOutputState();
    {
        Region r(ctx, "DebugIcons");
        m_pass->Execute(ctx.cl, fv.view.viewProjMatrixNoJitter, fv.view.viewMatrix,
                        m_toneMap->GetFinalOutputTexture(), entry, fv.vpWidth, fv.vpHeight);
    }
    m_toneMap->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
}

// ==== Frame Generation: HUD-less colour ====
bool FrameGenHudLessEffect::IsEnabled(const Context& ctx) const
{
    return m_toneMap && ctx.gfx->MainSurface().GetFrameGenActive() > 0;
}

void FrameGenHudLessEffect::Execute(Context& ctx)
{
    const RHI::ResourceState entry = m_toneMap->GetFinalOutputState();
    const RHI::Texture* sceneTex = m_toneMap->GetFinalOutputTexture();
    if (!sceneTex) return;
    ctx.gfx->PushBarrier(RHI::GPUBarrier::Image(sceneTex, entry, RHI::ResourceState::COPY_SRC), ctx.cl);
    ctx.gfx->MainSurface().CaptureHudLessColor(ctx.cl, *sceneTex);
    ctx.gfx->PushBarrier(RHI::GPUBarrier::Image(sceneTex, RHI::ResourceState::COPY_SRC, entry), ctx.cl);
}

// ==== Frame Generation: UI colour + alpha ====
bool FrameGenUILayerEffect::IsEnabled(const Context& ctx) const
{
    return m_ui && m_ui->enabled && !m_ui->GetDrawList().IsEmpty() && ctx.view;
}

void FrameGenUILayerEffect::Execute(Context& ctx)
{
    // See SurfaceDX12::BeginFrameGenUICapture for why duplicating the draw
    // beats compositing this layer back over the scene. Null: FG is not
    // running (the surface records nothing).
    ISurface& surface = ctx.gfx->MainSurface();
    if (const RHI::Texture* fgUI = surface.BeginFrameGenUICapture(ctx.cl))
    {
        {
            Region r(ctx, "FG_UILayer");
            m_ui->Execute(ctx.cl, fgUI, RHI::ResourceState::RENDERTARGET,
                          ctx.view->vpWidth, ctx.view->vpHeight, ctx.view->globalTimeSec);
        }
        surface.EndFrameGenUICapture(ctx.cl);
    }
}

// ==== UI ====
bool UIEffect::IsEnabled(const Context& ctx) const
{
    const uint32_t viewSlot = RHI::CurrentRecordingView().slot;
    return m_ui && m_ui->enabled && m_toneMap && !m_ui->ViewDrawList(viewSlot).IsEmpty() && ctx.view &&
           (viewSlot == 0 || (ctx.show & static_cast<uint32_t>(RG::ShowFlag::Hud)));
}

void UIEffect::Execute(Context& ctx)
{
    // The tracked state on entry: resize paths can leave UAV / SR_COMPUTE.
    const RHI::ResourceState entry = m_toneMap->GetFinalOutputState();
    {
        Region r(ctx, "UI");
        m_ui->Execute(ctx.cl, m_toneMap->GetFinalOutputTexture(), entry,
                      ctx.view->vpWidth, ctx.view->vpHeight, ctx.view->globalTimeSec,
                      m_ui->ViewDrawList(RHI::CurrentRecordingView().slot));
    }
    m_toneMap->SetFinalOutputState(RHI::ResourceState::SHADER_RESOURCE);
}

} // namespace PostProcess
