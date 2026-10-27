// Video/VideoFeature.cpp - the Video module's render feature.
//
// Two graph passes:
//   VideoPass (Atmosphere, between the height fog and the scene voxels): the first SCREEN-SPACE component, the
//     overlay / cinematic path, composited over the fogged HDR scene; a cinematic dims the world
//     with its alpha.
//   VideoQuadPass (Transparent, right after TransparentPass): the WORLD-SPACE quads, depth test on
//     and depth write off (the transparent / particle convention).
// Extract (the game half) picks the components and hands the quads over as values. The frame
// source puts each decoded frame into the component's NV12 texture on the graphics queue (a copy
// that waits on the decoder's fence, or an upload) before the passes sample it.

#include "Video/VideoFeature.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Renderer/PassSet.h"        // RegisterView
#include "Renderer/RenderFeature.h"
#include "RenderGraph/RenderPass/VideoPass.h"
#include "RenderGraph/RenderPass/VideoQuadPass.h"

#include <vector>

namespace
{
class VideoFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "Video"; }
    uint32_t    Stages() const override
    {
        return StageBit(RG::Stage::Atmosphere) | StageBit(RG::Stage::Transparent);
    }

    void AddPasses(RG::Stage stage, FeatureCompileContext& ctx) override
    {
        // Before, not After("HeightFogPass"): an After anchors at its target's registration, and the
        // height fog registers after every core pass, which would put the overlay at the stage's end.
        // Placed here, it follows the height fog (registered first) and precedes the volumetric fog
        // and the snapshots the RR guide and NPR read.
        if (stage == RG::Stage::Atmosphere)
            m_screenPass = ctx.AddPass(std::make_unique<VideoPass>(),
                { RG::Before("SceneVoxelPass", "the overlay stays unfogged, under the volumetric fog and inside "
                                               "the RR colour snapshot") });
        else if (stage == RG::Stage::Transparent)
        {
            m_quadPass = ctx.AddPass(std::make_unique<VideoQuadPass>(ctx.textures.depth),
                { RG::After("TransparentPass", "a quad behind a glass pane reads through it by the depth test; "
                                               "in front, it covers the glass") });
            ctx.passes.RegisterView("VideoQuad", m_quadPass);   // a secondary view's camera
        }
    }

    // A secondary view: the quads through its camera (the main view's comes with the extract).
    void PrepareView(FeatureFrameContext& ctx) override
    {
        if (m_quadPass) m_quadPass->SetViewCamera(ctx.view.view.viewProjMatrixNoJitter);
    }

    void Extract(FeatureExtractContext& ctx) override
    {
        if (!m_screenPass && !m_quadPass)
            return;
        World& world = ctx.world;

        const VideoComponent* screenActive = nullptr;
        world.ForEach<VideoComponent>(
            [&](Entity /*e*/, const VideoComponent& vc)
        {
            if (vc.worldSpace) return;
            if (screenActive)  return;
            if (vc.state == VideoPlaybackState::Stopped) return;
            if (vc.currentDpbSlot >= vc.dpb.size())      return;
            screenActive = &vc;
        });

        if (m_screenPass)
        {
            m_screenPass->SetActiveFrame(screenActive);
            if (screenActive)
            {
                m_screenPass->SetRect(screenActive->rectUMin, screenActive->rectVMin,
                                      screenActive->rectUMax, screenActive->rectVMax);
                m_screenPass->SetAlpha(screenActive->renderAlpha);
                m_screenPass->SetColorSpace(screenActive->colorSpace);
            }
        }
        if (m_quadPass)
        {
            // Values only, not a World pointer: Execute runs on a RenderGraph Phase-B TaskSystem worker,
            // where iterating the World would be a live-World read off the main thread. Nothing here
            // keeps a component alive.
            std::vector<VideoQuadPass::Quad> quads;
            quads.reserve(8);
            world.ForEach<VideoComponent>(
                [&](Entity e, const VideoComponent& vc)
            {
                if (!vc.worldSpace) return;
                if (vc.state == VideoPlaybackState::Stopped) return;
                if (vc.currentDpbSlot >= vc.dpb.size()) return;
                if (!vc.dpb[vc.currentDpbSlot].IsValid()) return;

                VideoQuadPass::Quad q;
                q.nv12  = vc.dpb[vc.currentDpbSlot];
                if (const auto* gt = world.GetComponent<GlobalTransform>(e))
                    q.world = gt->matrix;
                else
                    DirectX::XMStoreFloat4x4(&q.world, DirectX::XMMatrixIdentity());
                q.width      = vc.worldWidth;
                q.height     = vc.worldHeight;
                q.alpha      = vc.renderAlpha;
                q.colorSpace = vc.colorSpace;
                quads.push_back(q);
            });
            m_quadPass->SetQuads(std::move(quads));
            // This frame's VP: a stale one makes the quads swim against the scene on motion.
            m_quadPass->SetCamera(ctx.viewProjNoJitter);
        }
    }

private:
    VideoPass*     m_screenPass = nullptr;   // owned by the main graph
    VideoQuadPass* m_quadPass   = nullptr;   // owned by the main graph
};
}

std::unique_ptr<IRenderFeature> CreateVideoFeature()
{
    return std::make_unique<VideoFeature>();
}
