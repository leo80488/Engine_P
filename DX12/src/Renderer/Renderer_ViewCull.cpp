// Renderer::CullView - what one view draws out of this frame's packets (Renderer/ViewCull.h).
// The preview's world-cull lane and the reflection probe's faces call it.

#include "Renderer/Renderer.h"

#include "Graphics/SceneBVH.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

void Renderer::CullView(const RG::SceneView& view, const ViewCullRequest& req,
                        std::vector<DrawPacket>& out, ViewCullStats* stats) const
{
    const RG::ViewCull& vc = view.cull;
    const uint32_t viewBit = vc.viewBit;

    // ---- 1. The BVH query ----
    // The volume the view looks into. None culls nothing: every entity counts as visible and only the
    // author-intent filters below remain.
    // Do not query a tree that is not this frame's: BuildScene_CullBVH builds and refits it only while GPU
    // culling is on (empty when off from boot, stale positions when switched off later). Then no entity
    // has been asked this frame, which is the leafless answer.
    const bool bvhCurrent = m_bvhStampValid && m_bvhStampFrame == FrameViewRead().currentFrame;
    const bool cullNothing = vc.volume.kind == RG::CullVolume::Kind::None;
    m_viewCullVisible.clear();
    if (!bvhCurrent) { /* nothing to ask */ }
    else if (vc.volume.kind == RG::CullVolume::Kind::Planes)
        m_sceneBVH.FrustumCull(CullPlanesTo(vc.volume), m_viewCullVisible);
    else if (vc.volume.kind == RG::CullVolume::Kind::Box)
    {
        SceneBVH::AABB box;
        box.min = { vc.volume.boxMin[0], vc.volume.boxMin[1], vc.volume.boxMin[2] };
        box.max = { vc.volume.boxMax[0], vc.volume.boxMax[1], vc.volume.boxMax[2] };
        m_sceneBVH.QueryAABB(box, m_viewCullVisible);
    }
    // An entity-indexed stamp array, not an unordered_set, which would cost a heap allocation and a hash
    // probe per instance. ++stampVal is the whole per-call reset.
    ++m_viewCullStampVal;
    for (Entity e : m_viewCullVisible)
    {
        const size_t idx = static_cast<size_t>(e);
        if (idx >= m_viewCullStamp.size()) m_viewCullStamp.resize(idx + 1, 0u);
        m_viewCullStamp[idx] = m_viewCullStampVal;
    }
    const auto isBvhVisible = [this, cullNothing](Entity e) -> bool
    {
        if (cullNothing) return true;
        const size_t idx = static_cast<size_t>(e);
        return idx < m_viewCullStamp.size()
            && m_viewCullStamp[idx] == m_viewCullStampVal;
    };
    // The entities this view never draws (ViewCullRequest::exclude), stamped.
    ++m_viewCullExclStampVal;
    for (uint32_t i = 0; i < req.excludeCount && req.exclude; ++i)
    {
        const size_t idx = static_cast<size_t>(req.exclude[i]);
        if (idx >= m_viewCullExclStamp.size()) m_viewCullExclStamp.resize(idx + 1, 0u);
        m_viewCullExclStamp[idx] = m_viewCullExclStampVal;
    }
    const bool anyExcluded = req.excludeCount != 0 && req.exclude != nullptr;
    const auto isExcluded = [this](Entity e) -> bool
    {
        const size_t idx = static_cast<size_t>(e);
        return idx < m_viewCullExclStamp.size()
            && m_viewCullExclStamp[idx] == m_viewCullExclStampVal;
    };

    // ---- 2. Filter this frame's draw lists ----
    // Keep a packet when ANY of its instances survived: an instance range is a batch, and splitting one
    // would mean re-emitting instance records. Off-volume instances in a kept batch still rasterise and
    // are clipped.
    ViewCullStats st;
    st.bvhLeaves  = m_sceneBVH.GetLeafCount();
    st.bvhVisible = static_cast<uint32_t>(m_viewCullVisible.size());
    st.bvhCurrent = bvhCurrent;
    bool countingSecondary = false;
    // ---- Which packets the conservative branch carries ----
    // A packet whose entity has no BVH leaf is kept: it has not been culled, only not asked (why a
    // primitive-only scene keeps everything). The counters split that kept set in two:
    //   leaflessPrim  - draws one of the five procedural primitives, which have a computable world box
    //                   (unit local box x transform), so a per-packet plane test could cull them;
    //   leaflessOther - billboards, beams, afterimages, terrain: camera-facing or per-frame extents, left
    //                   conservative. Recorded only.
    // The *Kept counters count only the packets the conservative branch alone saved (one with a visible leaf
    // would survive a plane test too). The flavour comes from dp.meshDescriptorIndex, a property of the
    // packet, so at most one of the two flags is set per packet.
    // The primitives' mesh-descriptor slots are read once per cull. Render-half safe:
    // MeshManager::InitPrimitives uploads them at Compile with persistent=true and nothing rewrites them,
    // so this reads no World and no game-half-mutable state.
    uint32_t primSlots[static_cast<int>(PrimitiveMeshType::Count)];
    for (int t = 0; t < static_cast<int>(PrimitiveMeshType::Count); ++t)
        primSlots[t] = m_meshMgr.GetPrimitive(t).meshDescSlot;
    auto isPrimitiveDescSlot = [&primSlots](uint32_t slot) -> bool
    {
        if (slot == RHI::kInvalidBufferIndex) return false;
        for (uint32_t s : primSlots) if (s != RHI::kInvalidBufferIndex && s == slot)
            return true;
        return false;
    };
    auto filterList = [&](DrawList src)
    {
        st.candidates += static_cast<uint32_t>(src.size());
        for (const DrawPacket& dp : src)
        {
            if (!dp.renderInMainPass) continue;
            // The view bit: an author clears it on a helper they do not want in that view (kSecondaryViewBit
            // for the PiP).
            if (viewBit != 0 && (dp.viewMask & viewBit) == 0) continue;

            const uint32_t endSlot = (std::min)(dp.instanceOffset + dp.instanceCount,
                                                m_maxInstances);
            bool keep = false;
            bool anyPrimLeafless = false, anyOtherLeafless = false;
            bool anyExcludedHere = false, anyIncluded = !anyExcluded;
            // Hoisted: the flavour is a property of the packet (dp.meshDescriptorIndex), so at most one of the two
            // flavour flags can be set for a packet; the early exit below relies on that.
            const bool primFlavour = isPrimitiveDescSlot(dp.meshDescriptorIndex);
            for (uint32_t i = dp.instanceOffset; i < endSlot; ++i)
            {
                // i is a DRAW-LOCAL index - resolve through this frame's remap.
                const Entity e = EntityForDrawLocalInstance(i);
                // An excluded entity neither keeps the batch nor is leafless.
                if (anyExcluded)
                {
                    if (isExcluded(e)) { anyExcludedHere = true; continue; }
                    anyIncluded = true;
                }
                // Tested vs visible, as the main path's bvhVisibleMask / bvhTestedMask: an entity with no BVH leaf has
                // not been culled, only not asked, and is kept. With GPU culling off the whole tree is in that state
                // (BuildScene_CullBVH gathers only under m_gpuCullingEnabled): every packet is drawn and the GPU clips.
                // A leafless instance does not break the loop: the keep set is the same either way (keep iff any
                // instance is visible or leafless), and running on keeps the counters a function of the packet, not
                // of the instance order.
                SceneBVH::AABB unusedBox;
                if (bvhCurrent && m_sceneBVH.TryGetLeafAabb(e, unusedBox))
                {
                    if (isBvhVisible(e)) keep = true;
                }
                else if (primFlavour) anyPrimLeafless = true;
                else                  anyOtherLeafless = true;
                // Early exit once keep and the packet's one eligible flavour flag are both set: with primFlavour
                // hoisted, no later instance can change keep or the counters.
                if (keep && (anyPrimLeafless || anyOtherLeafless)) break;
            }
            // A batch made only of excluded entities is not this view's.
            if (anyExcluded && !anyIncluded)
            {
                ++st.excludedDropped;
                continue;
            }
            // The conservative branch: count the two populations separately.
            if (anyPrimLeafless)  ++st.leaflessPrim;
            if (anyOtherLeafless) ++st.leaflessOther;
            if (!keep && (anyPrimLeafless || anyOtherLeafless))
            {
                keep = true;
                if (anyPrimLeafless)  ++st.leaflessPrimKept;
                if (anyOtherLeafless) ++st.leaflessOtherKept;
            }
            if (!keep) continue;
            if (anyExcludedHere && !m_viewCullMixedWarned)
            {
                // A batch cannot be split: the excluded instance draws with it.
                m_viewCullMixedWarned = true;
                LOG_WARNING("[VIEW_CULL] view=%s: a kept batch (%u instances, mesh slot %u, material %u) holds an "
                            "entity the view excludes; it draws with the batch", view.name ? view.name : "?",
                            dp.instanceCount, dp.meshDescriptorIndex, dp.materialIndex);
            }

            out.push_back(dp);
            ++st.kept;
            if (countingSecondary) ++st.keptSecondary;
            if (req.rewriteViewMask) out.back().viewMask = 0xFFFFFFFFu;
            if (req.naturalFilters)
            {
                if (out.back().filter == DrawFilter::SecondaryView)
                    out.back().filter = DrawFilter::Opaque;
                else if (out.back().filter == DrawFilter::SecondaryViewBlend)
                    out.back().filter = DrawFilter::Transparent;
                else if (out.back().filter == DrawFilter::SecondaryViewCustom)
                    out.back().filter = DrawFilter::Custom;
            }
        }
    };
    // Order is the contract: the output keeps the request's list order.
    size_t firstTransparent = std::numeric_limits<size_t>::max();
    for (uint32_t l = 0; l < req.listCount && l < ViewCullRequest::kMaxLists; ++l)
    {
        const DrawFilter f = req.lists[l];
        if (firstTransparent == std::numeric_limits<size_t>::max() &&
            (f == DrawFilter::Transparent || f == DrawFilter::SecondaryViewBlend))
            firstTransparent = out.size();
        countingSecondary = req.secondary[l];
        filterList(GetDrawList(f));
    }
    countingSecondary = false;

    // ---- 3. Re-sort the transparent tail against this view ----
    // The scene's transparent order is back-to-front for the main camera; another view needs its own
    // order or blending is wrong wherever the two disagree.
    // Depth comes from the BVH, the only render-safe per-entity world position (World is off limits on
    // this half, the packet carries no transform, the CPU instance shadow covers static slots only). The
    // BVH is refit every frame, so a leaf AABB's centre includes animation. A packet with no leaf keeps
    // +inf and sorts to the back, the conservative end for a blended draw.
    if (req.sortTransparent && firstTransparent < out.size())
    {
        const size_t nT = out.size() - firstTransparent;
        m_viewCullDepth.assign(nT, std::numeric_limits<float>::infinity());
        for (size_t i = firstTransparent; i < out.size(); ++i)
        {
            SceneBVH::AABB box;
            const Entity e = EntityForDrawLocalInstance(out[i].instanceOffset);
            if (e == NullEntity || !bvhCurrent || !m_sceneBVH.TryGetLeafAabb(e, box)) continue;
            const DirectX::XMFLOAT3 c = box.Center();
            // Planar view depth, as the main sort uses: a squared Euclidean distance orders objects at equal view
            // depth by how far off-axis they are, which is not the order alpha blending needs.
            const float d = (c.x - vc.eye[0]) * vc.forward[0]
                          + (c.y - vc.eye[1]) * vc.forward[1]
                          + (c.z - vc.eye[2]) * vc.forward[2];
            // NaN-safe: a NaN breaks the comparator's strict weak ordering (undefined behaviour in std::sort).
            // +inf sorts to the back.
            m_viewCullDepth[i - firstTransparent] =
                std::isfinite(d) ? d : std::numeric_limits<float>::infinity();
        }
        // Sort indices, then permute once: DrawPacket is ~200 bytes, and recomputing the distance per
        // comparison would cost O(n log n) BVH lookups.
        m_viewCullOrder.resize(nT);
        for (uint32_t i = 0; i < static_cast<uint32_t>(nT); ++i)
            m_viewCullOrder[i] = i;
        const std::vector<float>& depth = m_viewCullDepth;
        std::stable_sort(m_viewCullOrder.begin(), m_viewCullOrder.end(),
                         [&depth](uint32_t a, uint32_t b)
                         { return depth[a] > depth[b]; });
        m_viewCullSorted.clear();
        m_viewCullSorted.reserve(nT);
        for (uint32_t i : m_viewCullOrder)
            m_viewCullSorted.push_back(out[firstTransparent + i]);
        std::copy(m_viewCullSorted.begin(), m_viewCullSorted.end(),
                  out.begin() + static_cast<ptrdiff_t>(firstTransparent));
    }

    // [VIEW_CULL] - each view's first cull, once: what the tree held and what
    // was kept (a view that keeps 0 of N, or whose tree is empty, says so).
    const char* name = view.name ? view.name : "?";
    if (std::find(m_viewCullLoggedNames.begin(), m_viewCullLoggedNames.end(), name)
        == m_viewCullLoggedNames.end())
    {
        m_viewCullLoggedNames.emplace_back(name);
        LOG_INFO("[VIEW_CULL] view=%s kind=%s viewBit=0x%x bvhCurrent=%d bvhLeaves=%u bvhVisible=%u candidates=%u "
                 "kept=%u secondary=%u leaflessKept=%u excludedDropped=%u", name, RG::CullVolumeKindName(vc.volume.kind),
                 viewBit, bvhCurrent ? 1 : 0, st.bvhLeaves, st.bvhVisible, st.candidates, st.kept, st.keptSecondary,
                 st.leaflessPrimKept + st.leaflessOtherKept, st.excludedDropped);
    }
    if (stats) *stats = st;
}
