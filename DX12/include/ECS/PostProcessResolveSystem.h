#pragma once

// PostProcessResolveSystem: resolves the post-process volume set and the gameplay override stack
// into PostProcess::Runtime::resolved each frame.
//
// Phase PreRender, registered after the camera-stack resolve systems so it reads the final blended
// view position (no distance-falloff jitter during camera transitions). Reads ECS state and writes
// view-local runtime data only: no structural side effects.

#include "ECS/ISystem.h"
#include "ECS/TickPhase.h"

class World;
struct FrameContext;

class PostProcessResolveSystem : public SystemInPhase<TickPhase::PreRender>
{
public:
    const char* GetName() const override { return "PostProcessResolveSystem"; }
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
};
