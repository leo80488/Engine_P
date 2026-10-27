#pragma once

// ShadowFrustumCompute - pure math helper for CSM shadow-caster retention.
//
// Holds the per-cascade light-frustum OBBs / AABBs and 6-plane approximations used to
// decide whether a scene AABB falls inside any shadow cascade, so off-screen casters
// survive culling. The cascades come from ShadowSystem, the single source of split /
// fit / texel-snap math; this class only inverts each cascade's light VP to recover
// the world volume it rasterizes.
//
// No GPU resources and no backend types, so it can be unit-tested in isolation.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <cstdint>

#include "RenderGraph/RenderPass/ShadowPass.h"   // kCascadeCount, kRetainedCullCascades
#include "Graphics/RenderTypes.h"                // RenderCamera, RenderView

class World;
class ShadowSystem;

class ShadowFrustumCompute
{
public:
    static constexpr int kCascadeCount = ShadowPass::kCascadeCount;

    // Cascades that rasterize mesh packets. ShadowPass draws only terrain into the last
    // cascade (terrainOnlyCascade), so a "will this be drawn into a cascade" test must stop
    // here; testing kCascadeCount admits casters for a cascade that never draws them.
    // Pinned to ShadowPass's constant so the two cannot drift.
    static constexpr int kCasterCascades = ShadowPass::kRetainedCullCascades;
    static_assert(kCasterCascades <= kCascadeCount,
                  "kCasterCascades must not exceed the cascade count");
    struct Planes { DirectX::XMVECTOR planes[6]; };

    // Builds the per-cascade light-frustum volumes for shadow-caster culling from
    // ShadowSystem's cascade matrices, used as-is: do not re-derive splits here, the
    // retention volumes must be the cascades that render. Run it before scene culling so
    // off-screen entities inside a cascade are kept as casters. Returns true when
    // @p shadowSys has a valid directional light and the volumes are filled.
    bool Compute(const ShadowSystem& shadowSys);

    // Tests @p bb against all kCascadeCount cascades, the terrain-only last one included: a
    // fast AABB-vs-AABB reject, then the conservative 6-plane test. Returns true if any
    // cascade intersects it. Callers that need to know which cascade hit (or to stop at
    // kCasterCascades) walk GetAABB / GetPlanes with AabbVs6Planes.
    bool IntersectsAny(const DirectX::BoundingBox& bb) const;

    bool IsValid() const { return m_valid; }

    // Per-cascade accessors for callers that test one cascade at a time; valid only when
    // IsValid() is true.
    const DirectX::BoundingOrientedBox& GetOBB(int i)      const { return m_obb[i];  }
    const DirectX::BoundingBox&         GetAABB(int i)     const { return m_aabb[i]; }
    const Planes&                       GetPlanes(int i)   const { return m_planes[i]; }

    // Tests @p bb against exactly 6 outward-normal @p planes. Conservative: never
    // false-rejects, may false-accept.
    static bool AabbVs6Planes(const DirectX::BoundingBox& bb,
                              const DirectX::XMVECTOR (&planes)[6]);

private:
    DirectX::BoundingOrientedBox m_obb[kCascadeCount]{};
    DirectX::BoundingBox         m_aabb[kCascadeCount]{};
    alignas(16) Planes           m_planes[kCascadeCount]{};
    bool                         m_valid = false;
};
