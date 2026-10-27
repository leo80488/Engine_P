#include "ECS/VideoSystem.h"
#include "ECS/VideoComponent.h"
#include "ECS/ECS.h"
#include "ECS/FrameContext.h"

#include "Graphics/IGraphicsDevice.h"

#include "System/Log.h"

#include <algorithm>

void VideoSystem::Update(World& world, const FrameContext& ctx)
{
    world.ForEach<VideoComponent>(
        [&](Entity /*e*/, VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Stopped) return;
        // No source: dpb[] is author-driven (LoadTestPattern, manual upload); the renderer keeps sampling
        // whatever currentDpbSlot points at.
        if (!vc.decodedFrameSource) return;
        if (vc.state != VideoPlaybackState::Playing) return;

        const double dt = static_cast<double>(ctx.deltaTime)
                        * static_cast<double>((std::max)(vc.playRate, 0.0f));
        vc.clockSeconds += dt;
        if (!vc.decodedFrameSource->Tick(m_gfx, vc, dt))
        {
            LOG_ERROR("VideoSystem: decoded frame source failed — stopping");
            vc.state = VideoPlaybackState::Stopped;
        }
    });
}
