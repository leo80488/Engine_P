// RenderGraph_Views.cpp - the secondary views' texture sets: binding, placing in the views' alias heaps,
// parking and releasing (RenderGraph.h, BindViewSet).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/GpuMemoryLedger.h"       // the heaps' bytes are no view's
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MemoryPolicy.h"
#include "Graphics/TransientAliasArena.h"   // the views' alias heaps
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <mutex>
#include <string>
#include <vector>

namespace RG
{
    // ---- Views ----

    bool RenderGraph::ViewShared(size_t id) const
    {
        return id < m_viewShared.size() && m_viewShared[id] != 0;
    }

    uint32_t RenderGraph::DemandedCount() const
    {
        uint32_t n = 0;
        for (const GraphTexture& gt : m_graphTextures)
            if (gt.residency == TextureResidency::OnDemand && gt.demanded) ++n;
        return n;
    }

    TransientAliasArena& RenderGraph::ViewScratchArena()
    {
        if (!m_viewScratch)
            m_viewScratch = std::make_unique<TransientAliasArena>(TransientAliasArena::Kind::Textures,
                                                                  "ViewAliasHeap.Scratch", /*shared*/true);
        return *m_viewScratch;
    }

    void RenderGraph::ReleaseViewHeaps()
    {
        if (MemoryPolicy::KeepViewHeaps()) return;
        for (uint32_t s = 1; s < RHI::kMaxViews; ++s)
            for (const ViewTexture& v : m_viewSets[s].textures)
                if (v.placed) return;   // a set still lives on them
        uint64_t bytes = ViewAliasHeapBytes();
        for (auto& arena : m_viewArenas)
            if (arena) arena->Release();
        if (m_viewScratch) m_viewScratch->Release();
        if (bytes) LOG_INFO("[RG_ALIAS] no secondary view: the views' alias heaps (%llu MiB) are released",
                            (unsigned long long)(bytes >> 20));
    }

    void RenderGraph::StashViewSet(ViewSet& set)
    {
        const bool secondary = &set != &m_viewSets[0];
        set.textures.resize(m_textures.size());
        if (secondary) m_viewSets[0].textures.resize(m_textures.size());
        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            const VirtualTexture& vt = m_textures[i];
            ViewTexture&          v  = set.textures[i];
            if (vt.imported)
            {
                v = {};   // bound per Execute
                continue;
            }
            // A shared texture is the main view's: its states go back to the main view's set.
            ViewTexture& to = secondary && ViewShared(i) ? m_viewSets[0].textures[i] : v;
            if (&to != &v) v = {};
            to.texture      = vt.texture;
            to.state        = vt.currentState;
            to.stencilState = vt.stencilState;
        }
        set.width  = m_physicalWidth;
        set.height = m_physicalHeight;
    }

    void RenderGraph::LoadViewSet(const ViewSet& set)
    {
        const bool secondary = &set != &m_viewSets[0];
        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            VirtualTexture& vt = m_textures[i];
            if (vt.imported) continue;
            const ViewSet& from = secondary && ViewShared(i) ? m_viewSets[0] : set;
            const ViewTexture v = i < from.textures.size() ? from.textures[i] : ViewTexture{};
            vt.texture      = v.texture;
            vt.currentState = v.state;
            vt.stencilState = v.stencilState;
        }
        m_physicalWidth  = set.width;
        m_physicalHeight = set.height;
    }

    void RenderGraph::BackBoundSet(IGraphicsDevice& gfx)
    {
        if (m_boundSet != 0) PlaceBoundSet(gfx);
        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            VirtualTexture& vt = m_textures[i];
            if (vt.imported || vt.texture.IsValid()) continue;
            // A secondary view never makes its own copy of a shared texture: nothing in the view
            // writes it, and the main view has none either (OnDemand, not demanded).
            if (m_boundSet != 0 && ViewShared(i)) continue;
            if (i < m_graphTextures.size() && m_graphTextures[i].residency == TextureResidency::OnDemand &&
                !m_graphTextures[i].demanded)
                continue;
            CreatePhysicalTexture(vt, gfx);
        }
    }

    void RenderGraph::PlaceBoundSet(IGraphicsDevice& gfx)
    {
        static const bool s_off = Sys::CommandLine::Get().Has("-rgaliasoff");
        ViewSet& set = m_viewSets[m_boundSet];
        if (s_off || set.textures.size() != m_textures.size()) return;
        if (!m_viewArenas[0])
        {
            m_viewArenas[0] = std::make_unique<TransientAliasArena>(TransientAliasArena::Kind::Textures, "ViewAliasHeap.Textures");
            m_viewArenas[1] = std::make_unique<TransientAliasArena>(TransientAliasArena::Kind::RenderTargets, "ViewAliasHeap.RenderTargets");
        }
        // A heap grew since this set was placed (another view needed more), or an on-demand texture was asked for
        // since: its placed textures go (deferred) and it is placed again on the current heaps, where the other
        // views are.
        if (set.placedTried &&
            (set.placedEpoch[0] != m_viewArenas[0]->Epoch() || set.placedEpoch[1] != m_viewArenas[1]->Epoch() ||
             set.placedDemand != DemandedCount()))
        {
            for (size_t i = 0; i < set.textures.size(); ++i)
            {
                if (!set.textures[i].placed) continue;
                if (m_textures[i].texture.IsValid()) gfx.DestroyTexture(m_textures[i].texture);
                m_textures[i].texture  = {};
                set.textures[i].placed = false;
            }
            set.placedTried = false;
        }
        if (set.placedTried) return;
        set.placedTried = true;
        set.placedDemand = DemandedCount();

        // Every one of them lives through the view's whole segment: packed one after another, each heap from 0
        // (the other views' sets pack from 0 too - that is the sharing).
        struct Placement
        {
            size_t             id;
            int                heap;
            uint64_t           offset;
            RHI::TextureDesc   desc;
            RHI::ResourceState layout;
        };
        std::vector<Placement> placements;
        uint64_t cursor[2] = {};
        {
            std::lock_guard<std::mutex> lock(m_aliasMutex);
            for (size_t i = 0; i < m_textures.size() && i < m_aliasPlan.size(); ++i)
            {
                const VirtualTexture& vt = m_textures[i];
                if (vt.imported || vt.texture.IsValid() || ViewShared(i)) continue;
                const AliasPin pin = m_aliasPlan[i].pin;
                const bool demanded = pin == AliasPin::OnDemand && i < m_graphTextures.size() &&
                                      m_graphTextures[i].demanded;
                if (pin != AliasPin::None && pin != AliasPin::OutsideUse && pin != AliasPin::FirstUseLoads &&
                    !demanded)
                    continue;
                Placement p{ i, 0, 0, {}, RHI::ResourceState::UNDEFINED };
                p.desc = PhysicalDesc(vt, p.layout);
                uint64_t size = 0, align = 0;
                if (!gfx.GetTextureFootprint(p.desc, size, align)) continue;
                p.heap = (RHI::HasFlag(p.desc.bindFlags, RHI::BindFlag::RENDER_TARGET) ||
                          RHI::HasFlag(p.desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL)) ? 1 : 0;
                p.offset          = TransientAliasArena::AlignUp(cursor[p.heap], align);
                cursor[p.heap]    = p.offset + size;
                placements.push_back(p);
            }
        }
        // No idle when a heap grows: the new heap is fresh memory, and the sets on the old one keep it alive
        // (each texture holds its heap) until they are placed again. The heaps are every view's, not this
        // one's VRAM (GpuLedger's view attribution).
        const uint32_t attributed = GpuLedger::SetViewAttribution(0);
        const bool heapOk[2] = { cursor[0] && gfx.EnsureAliasArena(*m_viewArenas[0], cursor[0]),
                                 cursor[1] && gfx.EnsureAliasArena(*m_viewArenas[1], cursor[1]) };
        GpuLedger::SetViewAttribution(attributed);
        uint32_t placed = 0;
        for (const Placement& p : placements)
        {
            if (!heapOk[p.heap]) continue;
            VirtualTexture& vt = m_textures[p.id];
            if (!gfx.CreateTextureInArena(p.desc, *m_viewArenas[p.heap], p.offset, vt.texture)) continue;
            vt.currentState = p.layout;
            if (vt.desc.isDepth) vt.stencilState = p.layout;
            set.textures[p.id].placed = true;
            ++placed;
        }
        set.placedEpoch[0] = m_viewArenas[0]->Epoch();
        set.placedEpoch[1] = m_viewArenas[1]->Epoch();
        LOG_INFO("[RG_ALIAS] view=%u placed=%u of %zu bytes=%llu+%llu heaps=%llu+%llu",
                 m_boundSet, placed, placements.size(), (unsigned long long)cursor[0], (unsigned long long)cursor[1],
                 (unsigned long long)m_viewArenas[0]->HeapSize(), (unsigned long long)m_viewArenas[1]->HeapSize());
    }

    void RenderGraph::ActivateBoundSet(IGraphicsDevice& gfx, RHI::CommandList cmd)
    {
        const ViewSet& set = m_viewSets[m_boundSet];
        for (size_t i = 0; i < set.textures.size() && i < m_textures.size(); ++i)
        {
            const VirtualTexture& vt = m_textures[i];
            if (!set.textures[i].placed || !vt.texture.IsValid()) continue;
            gfx.PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, &vt.texture), cmd);
            const bool depth = vt.desc.isDepth;
            if (!depth && !vt.hasRTV) continue;
            // In its write state (ParkViewSet, or created in it): D3D12's first operation on an aliased render
            // target or depth texture is a Discard (or a Clear / Copy).
            const RHI::ResourceState write = depth ? RHI::ResourceState::DEPTHSTENCIL : RHI::ResourceState::RENDERTARGET;
            if (!ENGINE_CHECK_ONCE(vt.currentState == write && (!vt.planar || vt.stencilState == write),
                                   "[RG_ALIAS] view %u: '%s' is not parked in its write state", m_boundSet,
                                   vt.name.c_str()))
                continue;
            gfx.DiscardTexture(vt.texture, cmd);
        }
    }

    RHI::CommandList RenderGraph::ParkViewSet(IGraphicsDevice& gfx, uint32_t slot, RHI::CommandList after)
    {
        if (slot == 0 || slot >= RHI::kMaxViews || slot == m_boundSet) return {};
        ViewSet& set = m_viewSets[slot];
        std::vector<RHI::GPUBarrier> barriers;
        for (size_t i = 0; i < set.textures.size() && i < m_textures.size(); ++i)
        {
            ViewTexture& v = set.textures[i];
            const VirtualTexture& vt = m_textures[i];
            if (!v.placed || !v.texture.IsValid() || (!vt.desc.isDepth && !vt.hasRTV)) continue;
            const RHI::ResourceState write = vt.desc.isDepth ? RHI::ResourceState::DEPTHSTENCIL
                                                             : RHI::ResourceState::RENDERTARGET;
            if (vt.planar)
            {
                if (v.state != write)
                    barriers.push_back(RHI::GPUBarrier::ImagePlane(&v.texture, 0, v.state, write));
                if (v.stencilState != write)
                    barriers.push_back(RHI::GPUBarrier::ImagePlane(&v.texture, 1, v.stencilState, write));
                v.stencilState = write;
            }
            else if (v.state != write)
                barriers.push_back(RHI::GPUBarrier::Image(&v.texture, v.state, write));
            v.state = write;
        }
        if (barriers.empty()) return {};
        RHI::CommandList cl = gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        gfx.SetCommandListName(cl, "RG.ParkViewSet");
        if (after.IsValid()) gfx.AddCommandListDependency(cl, after);
        gfx.PushBarriers(barriers.data(), static_cast<uint32_t>(barriers.size()), cl);
        return cl;
    }

    RenderGraph::ViewSetBytes RenderGraph::GetViewSetBytes(IGraphicsDevice& gfx, uint32_t slot) const
    {
        ViewSetBytes out;
        if (slot == 0 || slot >= RHI::kMaxViews) return out;
        const ViewSet& set = m_viewSets[slot];
        for (size_t i = 0; i < set.textures.size() && i < m_textures.size(); ++i)
        {
            const VirtualTexture& vt = m_textures[i];
            const ViewTexture&    v  = set.textures[i];
            const RHI::Texture& tex = (slot == m_boundSet) ? vt.texture : v.texture;
            if (vt.imported || ViewShared(i) || !tex.IsValid()) continue;
            uint64_t size = 0, align = 0;
            if (!gfx.GetTextureFootprint(tex.desc, size, align)) continue;
            if (v.placed) { out.placed += size; ++out.placedCount; }
            else          out.committed += size;
        }
        return out;
    }

    uint64_t RenderGraph::ViewAliasHeapBytes() const
    {
        const uint64_t graphHeaps = m_viewArenas[0] ? m_viewArenas[0]->HeapSize() + m_viewArenas[1]->HeapSize() : 0;
        return graphHeaps + (m_viewScratch ? m_viewScratch->HeapSize() : 0);
    }

    void RenderGraph::BindViewSet(IGraphicsDevice& gfx, uint32_t slot)
    {
        if (slot >= RHI::kMaxViews || slot == m_boundSet || !m_compiled) return;
        if (!ENGINE_CHECK(!m_executing, "RenderGraph::BindViewSet inside Execute")) return;
        StashViewSet(m_viewSets[m_boundSet]);
        m_boundSet = slot;
        ViewSet& set = m_viewSets[slot];
        if (slot != 0)
        {
            const uint32_t w = m_fixedWidth  ? m_fixedWidth  : gfx.GetRenderWidth();
            const uint32_t h = m_fixedHeight ? m_fixedHeight : gfx.GetRenderHeight();
            if (set.compile != m_compileCount || set.width != w || set.height != h ||
                set.textures.size() != m_textures.size())
            {
                // A new compile or size: the old textures go (deferred), new ones at the view's size.
                for (ViewTexture& v : set.textures)
                    if (v.texture.IsValid()) gfx.DestroyTexture(v.texture);
                set.textures.assign(m_textures.size(), ViewTexture{});
                set.width       = w;
                set.height      = h;
                set.compile     = m_compileCount;
                set.placedTried = false;
                std::string shared;
                for (size_t i = 0; i < m_textures.size(); ++i)
                    if (ViewShared(i))
                    {
                        if (!shared.empty()) shared += ' ';
                        shared += m_textures[i].name;
                    }
                LOG_INFO("[VIEW] slot=%u textures at %ux%u; the main view's: %s", slot, w, h,
                         shared.empty() ? "-" : shared.c_str());
            }
        }
        LoadViewSet(set);
        BackBoundSet(gfx);
    }

    void RenderGraph::ReleaseViewSet(IGraphicsDevice& gfx, uint32_t slot)
    {
        if (slot == 0 || slot >= RHI::kMaxViews) return;
        if (!ENGINE_CHECK(slot != m_boundSet, "RenderGraph::ReleaseViewSet: view %u is bound", slot)) return;
        ViewSet& set = m_viewSets[slot];
        for (ViewTexture& v : set.textures)
            if (v.texture.IsValid()) gfx.DestroyTexture(v.texture);
        set = ViewSet{};
    }
} // namespace RG
