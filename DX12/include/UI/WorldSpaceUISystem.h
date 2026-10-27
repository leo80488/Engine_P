#pragma once

// WorldSpaceUISystem - per-frame CPU computation for world-space UI entities:
//   * per WorldSpaceUIComponent, from the entity's GlobalTransform (already written by
//     TransformSystem / FollowSystem): camera-space depth, distance fade, the ConstantPixel
//     scale factor and the behind-camera / fade cull -> computedAlpha / computedScale /
//     isCulled
//   * DamageNumberComponent: integrates velocity, counts lifetime down, destroys spent ones
//   * world text: resolves fonts, bakes missing glyphs, resolves custom-shader slots
// No GPU work: the render pass reads the computed fields and emits geometry.

#include <DirectXMath.h>

class World;

namespace UI
{
    struct WorldSpaceUIView
    {
        DirectX::XMFLOAT4X4 viewProjMatrix; // un-jittered, row-major
        DirectX::XMFLOAT3   cameraRightWS;  // world-space unit right
        DirectX::XMFLOAT3   cameraUpWS;     // world-space unit up
        DirectX::XMFLOAT2   canvasSize{ 1.0f, 1.0f }; // viewport pixels (for ConstantPixel mode)
        bool                valid = false;
    };

    // A secondary view's camera for the tick (its slot, 1 .. WorldSpaceUIComponent::kViewCount).
    struct WorldSpaceUIViewSlot
    {
        uint32_t         slot = 0;
        WorldSpaceUIView view;
    };

    class WorldSpaceUISystem
    {
    public:
        // Update fade / scale / cull for every WorldSpaceUI entity, advance DamageNumber
        // lifetimes and make world text's glyphs resident. @p dt in seconds. @p views: the
        // secondary views this frame, whose fade / scale / cull go to the per-view outputs.
        void Tick(World& world, const WorldSpaceUIView& view, float dt,
                  const WorldSpaceUIViewSlot* views = nullptr, uint32_t viewCount = 0);
    };

} // namespace UI
