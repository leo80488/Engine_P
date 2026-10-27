#pragma once

// PostProcess resolve - turns the spatial volume set + the gameplay override
// stack into one flat ResolvedPostProcessSettings for a view:
//
//   result = Flatten(engine default profile)
//   gather volumes (layerMask + spatial hit) -> effective weight
//   stable sort by priority ascending (ties: iteration order)
//   for each: BlendProfileInto(result, profile, weight)   // sequential lerp
//   then the live overrides by priority, the same way     // weight = masterWeight x envelope
//
// A pure read of ECS state + a write of view-local data (PostProcess::Runtime),
// so it is safe in the PreRender phase after the camera blend resolves.

#include "PostProcess/PostProcessRuntime.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class World;

namespace PostProcess
{

// Signed distance (world units) from @p p to the boundary of a volume whose
// bounds come from @p worldMatrix. Negative inside, 0 on the surface, positive
// outside. `isBox` selects OBB vs sphere.
float DistanceToSurface(const DirectX::XMFLOAT4X4& worldMatrix,
                        bool isBox, const DirectX::XMFLOAT3& p);

// The settings a camera at @p cameraPos gets: the engine default, the volumes it is in (priority order) and the
// override stack on top, into @p out. Advances and writes nothing shared, so a secondary view's camera can ask
// as often as it likes; @p hits (optional) receives the volumes that counted.
void ResolveAt(World& world, const DirectX::XMFLOAT3& cameraPos, uint32_t viewLayerMask,
               ResolvedPostProcessSettings& out, std::vector<VolumeHitDebug>* hits = nullptr);

// Run the full resolve for one view and store the result in
// PostProcess::Runtime::Get().resolved. Advances + prunes the override stack by
// @p dt. @p viewLayerMask is AND-ed against each volume's layerMask.
void ResolveForView(World& world,
                    const DirectX::XMFLOAT3& cameraPos,
                    uint32_t viewLayerMask,
                    float dt);

} // namespace PostProcess
