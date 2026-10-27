// Renderer/Features/TransmissionFeature.cpp - BlendMode::Transmissive's raster half as a
// render feature.
//
// Two graph passes in the Transparent stage, right before TransparentPass
// (RenderGraph/RenderPass/TransmissionPasses.h): the prepass stamps what the background
// behind each Transmissive surface turns into, the apply writes that background, refracted,
// blurred and tinted, into the HDR scene. TransparentPass then adds the surfaces' own light
// (custom_surface.hlsli, TRANSMISSIVE_BLEND).
//   Prepare (main thread, before the graph records): on when the main view draws a
//     Transmissive packet, pass.transmission.enabled is set, the view is lit and
//     perspective. Off, nothing runs and TransparentPass falls back to a grey coverage
//     blend (ITransmissionComposite: LightCB.gTransmissionApplied).
//   Record at PreResolve, after the sky's velocity fill (VelocityFillFeature): the covered
//     pixels' velocity becomes the motion of the background they show
//     (TransmissionMotionPass), so TAA, FSR 2 and DLSS SR follow the refracted image.
//     Not under ray reconstruction, which is handed the colour before translucency, nor
//     with pass.transmission.motionVectors off.
//   The preview graphs draw no transmission: their colour pass takes the fallback. Nor do the
//     secondary views: the passes are not view-safe - the scene snapshot, the targets and
//     the motion history are the main view's - so a view's segment takes the fallback too.

#include "Renderer/RenderFeature.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/TransmissionPasses.h"
#include "Renderer/PassSet.h"
#include "Settings/PassSettings.h"

#include <memory>

namespace
{
class TransmissionFeature final : public IRenderFeature, public ITransmissionComposite
{
public:
    const char* Name() const override { return "Transmission"; }
    uint32_t    Stages() const override
    {
        return StageBit(RG::Stage::Transparent) | StageBit(RG::Stage::PreResolve);
    }
    // At PreResolve: after the sky's velocity fill (VelocityFillFeature, 200), which would
    // overwrite the motion of a pane in front of the sky.
    int         Order() const override { return 300; }

    void AddPasses(RG::Stage, FeatureCompileContext& ctx) override
    {
        RG::RenderGraph& graph = ctx.graph;
        m_graph = &graph;
        // On demand: backed on the first frame that draws a Transmissive surface (Prepare).
        m_targets[0] = graph.CreateTexture("Transmission_Params",
            { TransmissionPrepass::kParamsFormat, 0, 0, false, false, true }, RG::TextureResidency::OnDemand);
        m_targets[1] = graph.CreateTexture("Transmission_Tint",
            { TransmissionPrepass::kTintFormat, 0, 0, false, false, true }, RG::TextureResidency::OnDemand);
        m_targets[2] = graph.CreateTexture("Transmission_LayerDepth",
            { TransmissionPrepass::kLayerDepthFormat, 0, 0, false, false, true }, RG::TextureResidency::OnDemand);

        // Both Before rules, no After: an After anchors a pass after its target's registration, and
        // these register after the particles, which must keep following TransparentPass.
        m_prepass = ctx.AddPass(std::make_unique<TransmissionPrepass>(ctx.textures.depth, m_targets[0], m_targets[1],
                                                                      m_targets[2]),
                                { RG::Before("TransmissionApplyPass", "the apply reads its targets"),
                                  RG::Before("TransparentPass", "stamps what the colour pass draws over") });
        m_apply = ctx.AddPass(std::make_unique<TransmissionApplyPass>(graph, ctx.textures.depth, m_targets[0],
                                                                      m_targets[1], m_targets[2]),
                              { RG::Before("TransparentPass", "the background goes in before the surfaces' light") });
        ctx.passes.Register("TransmissionApply", m_apply);
    }

    void CreatePasses(FeatureCompileContext& ctx) override
    {
        m_motion = std::make_unique<TransmissionMotionPass>();
        m_motion->Init(ctx.gfx);
        ctx.passes.Register("TransmissionMotion", m_motion);
    }

    void Prepare(RG::Stage stage, FeatureFrameContext& ctx) override
    {
        if (stage != RG::Stage::Transparent || !m_prepass || !m_apply)
            return;
        m_inView = false;
        uint32_t draws = 0;
        if (ctx.draws)
            for (const DrawPacket& dp : *ctx.draws)
                if (TransmissionPrepass::IsTransmissivePacket(dp)) ++draws;
        const RenderCamera& camera = ctx.view.camera;
        m_applied = draws > 0 && Settings::Pass::transmissionEnabled() && ctx.sceneView.viewMode == 0 &&
                    !camera.orthographic && EnsureTargets(ctx.gfx) && m_apply->EnsureTargets(ctx.gfx);
        m_prepass->SetActive(m_applied);
        m_apply->SetActive(m_applied, { camera.nearZ, camera.farZ, camera.fov });
    }

    // A secondary view's segment follows: until the next frame's Prepare, nothing is applied.
    void PrepareView(FeatureFrameContext&) override { m_inView = true; }

    void Record(RG::Stage, FeatureRecordContext& ctx) override
    {
        if (!m_motion || m_inView)
            return;
        if (!m_applied || !m_prepass->Ran() || ctx.plan.rrRuns || !Settings::Pass::transmissionMotionVectors())
        {
            m_motion->ResetHistory();
            return;
        }
        // The apply read the prepass's targets in a pixel shader; the motion reads them in compute.
        for (const RG::RGTextureHandle h : { m_targets[0], m_targets[2] })
        {
            const RHI::ResourceState state = ctx.graph.GetTextureState(h);
            if (state == RHI::ResourceState::SHADER_RESOURCE || state == RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
                continue;
            ctx.gfx.PushBarrier(RHI::GPUBarrier::Image(ctx.graph.GetPhysicalTexture(h), state,
                                                       RHI::ResourceState::SHADER_RESOURCE_COMPUTE), ctx.cl);
            ctx.graph.SetTextureState(h, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        }
        TransmissionMotionPass::Inputs in;
        in.params        = ctx.graph.GetPhysicalTexture(m_targets[0]);
        in.layerDepth    = ctx.graph.GetPhysicalTexture(m_targets[2]);
        in.velocity      = ctx.graph.GetPhysicalTexture(ctx.textures.velocity);
        in.velocityState = ctx.graph.GetTextureState(ctx.textures.velocity);
        in.depthSrv      = ctx.resources.depthSrv;
        in.width         = ctx.view.renderWidth;
        in.height        = ctx.view.renderHeight;
        const uint32_t r = ctx.gfx.BeginGPUTimestamp(ctx.cl, "TransmissionMotion");
        const bool ran = m_motion->Execute(ctx.cl, in);
        ctx.gfx.EndGPUTimestamp(ctx.cl, r);
        if (ran) ctx.graph.SetTextureState(ctx.textures.velocity, RHI::ResourceState::UNORDERED_ACCESS);
        else     m_motion->ResetHistory();
    }

    bool TransmissionApplied() const override { return m_applied && !m_inView; }

private:
    // The prepass's targets, backed for good once asked for (RenderGraph::EnsurePhysicalTexture:
    // the serial window before the graph records, where Prepare runs). False when one cannot be.
    bool EnsureTargets(IGraphicsDevice& gfx)
    {
        for (const RG::RGTextureHandle h : m_targets)
        {
            m_graph->EnsurePhysicalTexture(h, gfx);
            if (!m_graph->GetPhysicalTexture(h))
                return false;
        }
        return true;
    }

    RG::RenderGraph*       m_graph   = nullptr;
    RG::RGTextureHandle    m_targets[3];          // params, tint, layer depth
    TransmissionPrepass*   m_prepass = nullptr;   // owned by the main graph
    TransmissionApplyPass* m_apply   = nullptr;
    std::unique_ptr<TransmissionMotionPass> m_motion;
    bool                   m_applied = false;
    bool                   m_inView  = false;     // a secondary view's segment is recording
};
}

REGISTER_RENDER_FEATURE(TransmissionFeature)
