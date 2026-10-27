#pragma once

// ECS::PostProcessVolumeComponent: makes an entity (with a Transform) a spatial post-process
// volume. The GlobalTransform is the only source of the bounds: Box is an OBB from position +
// rotation + scale (half-extents = scale), Sphere uses position + scale.x as radius.
//
// The look is not stored here: the volume references a shared PostProcessProfile by handle.
// profilePath is what is serialized; scene load resolves it to profile via ProfileSystem::Acquire.
// A new editor volume gets a runtime profile (ProfileSystem::CreateRuntime) until it is saved as a
// .ppprofile asset.

#include "PostProcess/PostProcessProfile.h"   // ProfileHandle

#include <cstdint>
#include <string>

namespace ECS
{

enum class PPVolumeShape : uint8_t
{
    Box,      // OBB from Transform position + rotation + scale (half-extents)
    Sphere,   // center + scale.x as radius
};

struct PostProcessVolumeComponent
{
    bool          isGlobal      = false;        // true = unbounded (ignores shape/bounds)
    PPVolumeShape shape         = PPVolumeShape::Box;
    float         priority      = 0.0f;         // higher blends later -> wins
    float         blendWeight   = 1.0f;         // 0..1 master weight
    float         blendDistance = 1.0f;         // world units; falloff shell outside the bounds
    uint32_t      layerMask     = 0xFFFFFFFFu;  // AND-ed with the view's layer mask

    PostProcess::ProfileHandle profile;         // -> shared PostProcessProfile resource
    std::string                profilePath;     // .ppprofile asset path (serialized)
};

} // namespace ECS
