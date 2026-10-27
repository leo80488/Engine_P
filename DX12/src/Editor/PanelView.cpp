#include "Editor/PanelView.h"

#include "Graphics/ViewSlot.h"   // RHI::kMaxViews
#include "Renderer/Renderer.h"
#include "System/Log.h"

#include <cstdio>

Editor::PanelView::Picture Editor::PanelView::Request(Renderer& renderer, Entity camera, std::uint32_t w,
                                                      std::uint32_t h, std::uint32_t show)
{
    Picture p;
    if (camera == NullEntity || w == 0 || h == 0)
    {
        // Nothing to look through: not requested, so the boundary hands a live view back.
        p.status = "no camera";
        return p;
    }
    m_requested = true;

    if (m_slot == 0)
    {
        m_slot = renderer.AcquireView(m_name, w, h);
        if (m_slot == 0)
        {
            m_requested = false;
            char text[96];
            std::snprintf(text, sizeof(text), "no free secondary view (at most %u), or no video memory left",
                          RHI::kMaxViews - 1);
            p.status = text;
            return p;
        }
        m_lease = renderer.GetViewInfo(m_slot).lease;
        LOG_INFO("[GAMEVIEW] %s view %d acquired", m_name, ViewId());
    }

    // Pushed every frame: the game half resolves the camera entity once a frame, and a camera latched on change
    // only would follow a deleted entity across a reload that reused its id.
    renderer.SetViewCamera(m_slot, camera, /*followMain=*/false, w, h, show);

    const Renderer::ViewInfo info = renderer.GetViewInfo(m_slot);
    p.srv = renderer.GetViewOutputSrv(m_slot);
    p.w   = info.width;
    p.h   = info.height;
    if (!p.srv) p.status = "rendering...";
    return p;
}

int Editor::PanelView::ViewId() const
{
    return m_slot ? static_cast<int>(Renderer::kSecondaryViewIdBase + m_slot) : -1;
}

void Editor::PanelView::ProcessPendingActions(Renderer* renderer)
{
    // Read before this frame's UI: "nothing asked for it in the last frame".
    const bool asked = m_requested;
    m_requested = false;
    if (m_slot != 0 && !asked && renderer)
        Release(*renderer, "not shown last frame");
}

void Editor::PanelView::ReleaseAtShutdown(Renderer* renderer)
{
    m_requested = false;
    if (m_slot != 0 && renderer)
        Release(*renderer, "shutdown");
}

void Editor::PanelView::ReleaseNow(Renderer* renderer, const char* why)
{
    m_requested = false;
    if (m_slot != 0 && renderer)
        Release(*renderer, why);
}

void Editor::PanelView::Release(Renderer& renderer, const char* why)
{
    LOG_INFO("[GAMEVIEW] %s view %d released (%s)", m_name, ViewId(), why);
    renderer.ReleaseView(m_slot);
    m_slot  = 0;
    m_lease = 0;
}
