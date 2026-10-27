#pragma once

// The textures a pass owns, described once.
//
// RG::PassTexture is the pass's object for one texture. Its size follows a policy over the
// frame's sizes (RG::PrepareContext: the render, viewport and post sizes, over a divisor) or
// is fixed; Ensure makes a new texture only when that resolves to a new size. Its state is
// kept in ONE book, the device's (IGraphicsDevice::QueryTextureState): the pass holds no
// ResourceState member. Imported Graph-tracked, the graph moves it; outside the graph its
// owner moves it with RG::TransitionTo.
//
// RG::HistoryTexture: planes of two PassTextures that flip together - Cur, the texture this
// frame writes, and Prev, the one written last. BeginFrame (serial, before anything binds
// them) flips only when the frame before wrote (MarkWritten), so Cur names one texture for
// the whole frame, inside the graph and out of it; a flip at the end of the writer would
// change what Cur means halfway through the frame. Whether the content is usable stays the
// owner's flag (the view's history).

#include "Graphics/GraphicsStruct.h"
#include "RenderGraph/PassLifecycle.h"
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

class IGraphicsDevice;

namespace RG
{
    // What a pass texture's size follows (PrepareContext).
    enum class SizePolicy : uint8_t
    {
        Render,    // renderW x renderH: the HDR / GBuffer size
        Display,   // viewportW x viewportH
        PostAA,    // postW x postH: the render size, or the display's once an upscaler resolved
        Fixed,     // the desc's width x height
    };

    struct PassTextureDesc
    {
        RHI::Format        format    = RHI::Format::UNKNOWN;
        SizePolicy         size      = SizePolicy::Render;
        uint32_t           divisor   = 1;   // the policy's size as (n + d - 1) / d
        uint32_t           width     = 0;   // Fixed
        uint32_t           height    = 0;
        uint32_t           mips      = 1;
        RHI::BindFlag      bind      = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        RHI::ResourceState initial   = RHI::ResourceState::UNORDERED_ACCESS;   // the state it is created in
        // The D3D12 object's name, the barrier journal's (a HistoryTexture
        // appends [0] / [1]); nullptr leaves the texture unnamed.
        const char*        debugName = nullptr;
    };

    class PassTexture
    {
    public:
        PassTexture() = default;
        explicit PassTexture(const PassTextureDesc& desc, std::string name = {});

        const PassTextureDesc& Desc() const { return m_desc; }

        // The size the policy resolves to for @p c.
        void Resolve(const PrepareContext& c, uint32_t& w, uint32_t& h) const;
        // A texture at the policy's size for @p c when there is none or that
        // size changed; true = a new texture, without the old one's content.
        bool Ensure(IGraphicsDevice& gfx, const PrepareContext& c);
        // The same at a size the caller resolved.
        bool EnsureSize(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
        // A new texture at @p w x @p h whatever it has now (false: creation failed).
        bool Rebuild(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
        // Destroys it (deferred: the GPU may still read it).
        void Release(IGraphicsDevice& gfx);

        const RHI::Texture& Tex() const { return m_tex; }
        bool     IsValid() const { return m_tex.IsValid(); }
        uint32_t Width()   const { return m_w; }
        uint32_t Height()  const { return m_h; }

    private:
        PassTextureDesc m_desc;
        std::string     m_name;   // what CreateTexture names it (outlives the call)
        RHI::Texture    m_tex;
        uint32_t        m_w = 0, m_h = 0;
    };

    // Moves @p tex to @p state on @p cl from the state the device's book holds, and records
    // nothing when it is there already. Only for a texture the graph does not track, recorded by
    // the pass that owns it: a Graph-tracked import is the graph's to move (Phase A records every
    // pass's graph barriers before any pass body runs, so a move made here would reach the book
    // after them). @p plane 0 / 1 moves one plane of a depth-stencil texture; -1 moves every
    // plane, each from its own state when they differ.
    void TransitionTo(IGraphicsDevice& gfx, RHI::CommandList cl, const RHI::Texture& tex, RHI::ResourceState state,
                      int plane = -1);

    class HistoryTexture
    {
    public:
        // One plane per desc, all flipping with the first. Not copyable: the
        // graph's import sources point into it (CurSource / PrevSource).
        HistoryTexture(std::initializer_list<PassTextureDesc> planes);
        HistoryTexture(const HistoryTexture&) = delete;
        HistoryTexture& operator=(const HistoryTexture&) = delete;

        uint32_t PlaneCount() const { return static_cast<uint32_t>(m_planes.size()); }

        // New textures for every plane, slot 0 then slot 1 of each in turn, at
        // the size @p c resolves (EnsureSize / Rebuild: the caller's): Cur is
        // slot 0 again and nothing is written. Ensure and EnsureSize do it only
        // when a plane has no textures or the size changed; true = they did
        // (the owner restarts its history).
        bool Ensure(IGraphicsDevice& gfx, const PrepareContext& c);
        bool EnsureSize(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
        void Rebuild(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
        void Release(IGraphicsDevice& gfx);

        // The frame's start, serial, before anything binds Cur or Prev: flips
        // when the frame before wrote.
        void BeginFrame();
        // The owner wrote Cur this frame (after its last dispatch).
        void MarkWritten() { m_written = true; }
        bool WrittenThisFrame() const { return m_written; }

        const RHI::Texture& Cur(uint32_t plane = 0) const;
        const RHI::Texture& Prev(uint32_t plane = 0) const;
        // The newest content: Cur once this frame wrote it, Prev until then.
        const RHI::Texture& LastWritten(uint32_t plane = 0) const { return m_written ? Cur(plane) : Prev(plane); }
        // Every plane has both its textures.
        bool IsValid() const;
        uint32_t Width()  const { return m_planes.empty() ? 0 : m_planes[0].tex[0].Width(); }
        uint32_t Height() const { return m_planes.empty() ? 0 : m_planes[0].tex[0].Height(); }

        // What the graph imports (ImportDeclarer::ImportHistory): copies of Cur
        // and Prev that BeginFrame, the rebuilds and Release keep current - the
        // graph reads them at each Execute.
        const RHI::Texture* CurSource(uint32_t plane = 0) const;
        const RHI::Texture* PrevSource(uint32_t plane = 0) const;

    private:
        struct Plane
        {
            PassTexture  tex[2];
            RHI::Texture curSource;
            RHI::Texture prevSource;
        };

        void RefreshSources();

        std::vector<Plane> m_planes;
        uint32_t           m_cur     = 0;
        bool               m_written = false;
    };
}
