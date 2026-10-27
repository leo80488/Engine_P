#pragma once

// Drives every VideoComponent's playback clock and frame source, in GameplayPreLogic (before AI,
// animation and render). Per entity and tick, while Playing: advance clockSeconds by deltaTime x
// playRate, then Tick() its decodedFrameSource, which decodes up to the clock and writes dpb[]. Stopped
// and Paused components and those without a source are left alone; the renderer keeps sampling
// dpb[currentDpbSlot].

#include "ECS/ISystem.h"

class IGraphicsDevice;

class VideoSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    explicit VideoSystem(IGraphicsDevice& gfx) : m_gfx(gfx) {}

    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "VideoSystem"; }

    // Mutates VideoComponent (clock, dpb[]) and the frame source submits GPU work: Opaque serialises it
    // against any system that reads VideoComponent on a worker.
    void DeclareAccess(SystemAccessBuilder& b) const override { b.OpaqueAccess(); }

private:
    IGraphicsDevice& m_gfx;
};
