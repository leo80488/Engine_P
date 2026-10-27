#pragma once
// Renderer/ViewCull.h - what one view draws out of this frame's packets.
//
// A view says where it looks in its RG::SceneView (SceneView::cull: an eye, a
// forward, a volume, the ViewBit it draws); Renderer::CullView asks the scene BVH
// with the volume and keeps, of the draw lists the request names, the packets the
// view can see - one rule set for every view that culls the world on the CPU (the
// world-cull preview lane - the PiP, the camera preview, the named camera capture -
// and the reflection probe's faces):
//
//   tested vs visible    an entity with a BVH leaf is kept when the volume holds its
//                        leaf; an entity with no leaf has not been asked and is kept
//                        (primitives, billboards, terrain - and every entity while
//                        GPU culling is off, which builds no tree);
//   a packet is a batch  kept when ANY of its instances is (an instance range cannot
//                        be split without re-emitting instance records); the kept
//                        batch's off-volume instances rasterise and clip;
//   author intent        renderInMainPass == 0 (a shadow-only proxy) is never drawn,
//                        and the packet's viewMask must hold the view's bit.
//
// The main view's CPU coarse cull is BuildScene_CullBVH (its result is the classify
// masks on the game half; its per-instance cull is the GPU's).

#include "Graphics/RenderTypes.h"   // DrawFilter, FrustumPlanes
#include "RenderGraph/SceneView.h"

#include <cstdint>

using Entity = std::uint32_t;   // ECS/ECS.h

// The lists one CullView call reads, in the order its output keeps them: a
// filter's packets must stay one contiguous run (RenderContext::GetDrawList
// finds a filter's span by scanning for it).
struct ViewCullRequest
{
    static constexpr uint32_t kMaxLists = 7;
    DrawFilter lists[kMaxLists] = {};
    uint32_t   listCount = 0;
    // The lists counted in ViewCullStats::keptSecondary as the secondary views'
    // duplicates (copies of candidates the main frustum rejected - the PiP's
    // SecondaryView / SecondaryViewBlend, the probe's ProbeView).
    bool       secondary[kMaxLists] = {};
    // Survivors' filter: SecondaryView -> Opaque, SecondaryViewBlend -> Transparent and
    // SecondaryViewCustom -> Custom - the passes of a preview graph ask for the natural filters.
    bool       naturalFilters = false;
    // Survivors' viewMask -> ~0u: a preview graph's GBufferPass tests
    // ViewBit::MainCamera, and the copy is the view's own.
    bool       rewriteViewMask = false;
    // Re-sort the Transparent / SecondaryViewBlend survivors back to front by
    // the planar depth along SceneView::cull.forward from cull.eye (an entity's
    // BVH leaf centre; one with no leaf sorts to the back).
    bool       sortTransparent = false;
    // Entities this view never draws (a probe face: every reflection probe's
    // own primitive - the editor's viz ball). They do not count for the keep
    // test, so a packet made only of them is dropped; a kept batch that also
    // holds one draws it (a batch cannot be split) and says so once.
    const Entity* exclude      = nullptr;
    uint32_t      excludeCount = 0;
};

// What one CullView call saw (the preview's cull counters among them).
struct ViewCullStats
{
    uint32_t bvhLeaves         = 0;   // the tree's leaf count
    uint32_t bvhVisible        = 0;   // entities the volume held
    uint32_t candidates        = 0;   // packets in the lists read
    uint32_t kept              = 0;   // packets kept
    uint32_t keptSecondary     = 0;   // ...of them from the lists marked secondary
    uint32_t leaflessPrim      = 0;   // packets with an instance of a procedural primitive and no BVH leaf
    uint32_t leaflessOther     = 0;   // packets with another leafless instance (billboard, beam, terrain)
    uint32_t leaflessPrimKept  = 0;   // ...kept only because of it
    uint32_t leaflessOtherKept = 0;
    uint32_t excludedDropped   = 0;   // packets dropped because every instance was excluded
    bool     bvhCurrent        = true;   // the tree was built / refit for this frame (else: none asked)
};

// FrustumPlanes <-> RG::CullVolume's planes (the same floats both ways).
inline void CullPlanesFrom(const FrustumPlanes& fp, RG::CullVolume& v)
{
    v.kind = RG::CullVolume::Kind::Planes;
    for (int i = 0; i < 6; ++i)
    {
        v.planes[i][0] = fp[i].normal.x;
        v.planes[i][1] = fp[i].normal.y;
        v.planes[i][2] = fp[i].normal.z;
        v.planes[i][3] = fp[i].distance;
    }
}
inline FrustumPlanes CullPlanesTo(const RG::CullVolume& v)
{
    FrustumPlanes fp{};
    for (int i = 0; i < 6; ++i)
    {
        fp[i].normal   = { v.planes[i][0], v.planes[i][1], v.planes[i][2] };
        fp[i].distance = v.planes[i][3];
    }
    return fp;
}
