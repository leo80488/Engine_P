#include "RenderGraph/PassTexture.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "System/Check.h"
#include "System/Log.h"

namespace RG
{
    HistoryHandles ImportDeclarer::ImportHistory(const char* name, const RGTextureDesc& desc,
                                                 const HistoryTexture& history, uint32_t plane)
    {
        HistoryHandles h;
        h.cur = Import(name, desc, { history.CurSource(plane), ImportEntry::Stale, ImportTracking::Graph });
        const std::string prev = std::string(name ? name : "") + ".Prev";
        h.prev = Import(prev.c_str(), desc, { history.PrevSource(plane), ImportEntry::History, ImportTracking::Graph });
        return h;
    }

    PassTexture::PassTexture(const PassTextureDesc& desc, std::string name)
        : m_desc(desc), m_name(name.empty() && desc.debugName ? std::string(desc.debugName) : std::move(name))
    {}

    void PassTexture::Resolve(const PrepareContext& c, uint32_t& w, uint32_t& h) const
    {
        uint32_t bw = 0, bh = 0;
        switch (m_desc.size)
        {
        case SizePolicy::Render:  bw = c.renderW;   bh = c.renderH;   break;
        case SizePolicy::Display: bw = c.viewportW; bh = c.viewportH; break;
        case SizePolicy::PostAA:  bw = c.postW;     bh = c.postH;     break;
        case SizePolicy::Fixed:   w = m_desc.width; h = m_desc.height; return;
        }
        const uint32_t d = m_desc.divisor ? m_desc.divisor : 1;
        w = (bw + d - 1) / d;
        h = (bh + d - 1) / d;
    }

    bool PassTexture::Ensure(IGraphicsDevice& gfx, const PrepareContext& c)
    {
        uint32_t w = 0, h = 0;
        Resolve(c, w, h);
        return EnsureSize(gfx, w, h);
    }

    bool PassTexture::EnsureSize(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
    {
        if (m_tex.IsValid() && w == m_w && h == m_h) return false;
        Rebuild(gfx, w, h);
        return true;
    }

    bool PassTexture::Rebuild(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
    {
        Release(gfx);
        if (w == 0 || h == 0) return false;
        RHI::TextureDesc td{};
        td.width      = w;
        td.height     = h;
        td.mipLevels = m_desc.mips;
        td.format     = m_desc.format;
        td.bindFlags = m_desc.bind;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = m_desc.initial;
        td.debugName = m_name.empty() ? nullptr : m_name.c_str();
        if (!gfx.CreateTexture(td, m_tex))
        {
            LOG_ERROR("RG::PassTexture: failed to create %s (%ux%u)", m_name.empty() ? "<unnamed>" : m_name.c_str(), w, h);
            m_tex = {};
            return false;
        }
        m_w = w;
        m_h = h;
        return true;
    }

    void PassTexture::Release(IGraphicsDevice& gfx)
    {
        if (m_tex.IsValid()) gfx.DestroyTexture(m_tex);   // deferred internally
        m_tex = {};
        m_w = m_h = 0;
    }

    void TransitionTo(IGraphicsDevice& gfx, RHI::CommandList cl, const RHI::Texture& tex, RHI::ResourceState state,
                      int plane)
    {
        if (!tex.IsValid()) return;
        // A depth-stencil texture whose planes differ: each from its own state.
        if (plane < 0 && RHI::HasStencilPlane(tex.desc.format) &&
            gfx.QueryTextureState(tex, 0) != gfx.QueryTextureState(tex, 1))
        {
            TransitionTo(gfx, cl, tex, state, 0);
            TransitionTo(gfx, cl, tex, state, 1);
            return;
        }
        const RHI::ResourceState have = gfx.QueryTextureState(tex, plane);
        if (have == RHI::ResourceState::UNDEFINED)
        {
            // The name only here: QueryTextureName reads the object's private data.
            ENGINE_CHECK_ONCE(false, "RG::TransitionTo: the device's book names no single state for '%s' - not moved",
                              gfx.QueryTextureName(tex).c_str());
            return;
        }
        if (have == state) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(&tex, have, state, -1, -1, plane), cl);
    }

    // ==== HistoryTexture ====
    HistoryTexture::HistoryTexture(std::initializer_list<PassTextureDesc> planes)
    {
        m_planes.reserve(planes.size());   // fixed from here on: the import sources point into it
        for (const PassTextureDesc& d : planes)
        {
            Plane& p = m_planes.emplace_back();
            const std::string base = d.debugName ? d.debugName : "";
            p.tex[0] = PassTexture(d, base.empty() ? std::string() : base + "[0]");
            p.tex[1] = PassTexture(d, base.empty() ? std::string() : base + "[1]");
        }
    }

    bool HistoryTexture::Ensure(IGraphicsDevice& gfx, const PrepareContext& c)
    {
        if (m_planes.empty()) return false;
        uint32_t w = 0, h = 0;
        m_planes[0].tex[0].Resolve(c, w, h);
        return EnsureSize(gfx, w, h);
    }

    bool HistoryTexture::EnsureSize(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
    {
        if (IsValid() && w == Width() && h == Height()) return false;
        Rebuild(gfx, w, h);
        return true;
    }

    void HistoryTexture::Rebuild(IGraphicsDevice& gfx, uint32_t w, uint32_t h)
    {
        for (Plane& p : m_planes)
            for (PassTexture& t : p.tex)
                t.Rebuild(gfx, w, h);
        m_cur     = 0;
        m_written = false;
        RefreshSources();
    }

    void HistoryTexture::Release(IGraphicsDevice& gfx)
    {
        for (Plane& p : m_planes)
            for (PassTexture& t : p.tex)
                t.Release(gfx);
        m_cur     = 0;
        m_written = false;
        RefreshSources();
    }

    void HistoryTexture::BeginFrame()
    {
        if (m_written) m_cur = 1u - m_cur;
        m_written = false;
        RefreshSources();
    }

    const RHI::Texture& HistoryTexture::Cur(uint32_t plane) const
    {
        static const RHI::Texture kNone{};
        return plane < m_planes.size() ? m_planes[plane].tex[m_cur].Tex() : kNone;
    }

    const RHI::Texture& HistoryTexture::Prev(uint32_t plane) const
    {
        static const RHI::Texture kNone{};
        return plane < m_planes.size() ? m_planes[plane].tex[1u - m_cur].Tex() : kNone;
    }

    bool HistoryTexture::IsValid() const
    {
        for (const Plane& p : m_planes)
            if (!p.tex[0].IsValid() || !p.tex[1].IsValid()) return false;
        return !m_planes.empty();
    }

    const RHI::Texture* HistoryTexture::CurSource(uint32_t plane) const
    {
        return plane < m_planes.size() ? &m_planes[plane].curSource : nullptr;
    }

    const RHI::Texture* HistoryTexture::PrevSource(uint32_t plane) const
    {
        return plane < m_planes.size() ? &m_planes[plane].prevSource : nullptr;
    }

    void HistoryTexture::RefreshSources()
    {
        for (Plane& p : m_planes)
        {
            p.curSource  = p.tex[m_cur].Tex();
            p.prevSource = p.tex[1u - m_cur].Tex();
        }
    }
}
