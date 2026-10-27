#pragma once

// PanelView - one editor panel's secondary view of the main pipeline (Renderer::AcquireView): the scene through a
// camera entity at a size, recorded by the same passes as the main view, for a panel that shows it this frame.
// The panel asks in every frame it draws (Request, UI pass); a frame without a request hands the view back at the
// next frame boundary (ProcessPendingActions), so a hidden or closed panel renders nothing and holds no slot.
// Editor-only: compiled into EditorCore.

#include "ECS/ECS.h"   // Entity

#include <cstdint>
#include <string>

class Renderer;

namespace Editor
{
    class PanelView
    {
    public:
        struct Picture
        {
            std::uint64_t srv = 0;   // 0 = nothing to show yet
            std::uint32_t w   = 0;   // the size the picture was rendered at
            std::uint32_t h   = 0;
            std::string   status;    // why there is no picture ("" = there is one)
        };

        // @p name is a string literal: the view's name (Render.views()) and the log's.
        explicit PanelView(const char* name) : m_name(name) {}

        // This frame's picture of @p camera at @p w x @p h, showing @p show (RG::ShowFlag bits). The view is made
        // at the next frame boundary and renders from the frame after: the first pictures come a frame or two
        // later. UI pass.
        Picture Request(Renderer& renderer, Entity camera, std::uint32_t w, std::uint32_t h, std::uint32_t show);

        // Frame boundary (EditorLayer::ProcessPendingActions): hands the view back when nothing asked for it in
        // the last frame.
        void ProcessPendingActions(Renderer* renderer);
        // At shutdown, where the per-frame release cannot run (it needs a next frame).
        void ReleaseAtShutdown(Renderer* renderer);
        // At a frame boundary, whether or not it was asked for: the caller knows it will not be.
        void ReleaseNow(Renderer* renderer, const char* why);

        // The view's id where Render.views() and a capture {view, lease} name it, and its lease; -1 / 0 without one.
        int           ViewId() const;
        std::uint32_t Lease() const { return m_lease; }
        bool          Live() const { return m_slot != 0; }

    private:
        void Release(Renderer& renderer, const char* why);

        // ---- State ----
        const char*   m_name;
        std::uint32_t m_slot      = 0;       // Renderer::AcquireView's (0 = none)
        std::uint32_t m_lease     = 0;
        bool          m_requested = false;   // asked for since the last frame boundary
    };
}
