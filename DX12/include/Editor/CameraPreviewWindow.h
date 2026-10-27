#pragma once

// Dockable editor panel showing a live secondary view of the scene through a chosen
// CameraComponent / VirtualCameraComponent. It selects an entity and a size, nothing more: the
// camera is resolved on the game half by Renderer::ExtractPreviewCameras, at one point in the frame
// that the classify pass and the extract can both be ordered against. (The panel may read World:
// GatherCameras iterates the camera pools every frame, legal because OnUIRender runs inside a
// Sys::ThreadCtx::Exempt scope on the serial-pinned editor host.) The draw source is the render
// half's second frustum cull; the picture arrives as an SRV re-resolved every frame.
// UI invariants:
//   1. It never writes main-viewport input state: it has no camera controls (the camera is the
//      scene's), so it never touches m_vpMouseDX/DY, m_viewportHovered or another viewport input
//      singleton.
//   2. The SRV is re-resolved every frame and never cached: descriptor slots recycle through a free
//      list and ImGui's own SRVs come from the same heap, so Renderer::GetPreviewSrv is called
//      once, at draw time.
//   3. Size is quantised and debounced: a resize recompiles the graph and recreates seven physical
//      textures, which unquantised dragging would do once per pixel. Width goes in 16-pixel steps
//      with a fixed 16:9 height (a scene view is not square, unlike the material inspector's).
//   4. It keeps its own per-frame state and resets it every frame instead of inheriting the main
//      viewport's collapsed-tab input latch (EditorLayer::RenderViewportPanel).
// Editor-only: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"                          // Entity
#include "Renderer/Preview/PreviewTypes.h"    // Preview::ViewDesc

#include <cstdint>
#include <string>
#include <vector>

class Renderer;
class World;

namespace Editor
{
    class CameraPreviewWindow
    {
    public:
        static CameraPreviewWindow& Get();

        // Draws the panel; called from EditorLayer::OnUIRender behind m_showCameraPreview, like every
        // dockable window. @p open is the menu-item flag (the window's X button clears it);
        // @p editorSelection drives the sync-to-selection option.
        void Draw(Renderer* renderer, World* world, bool* open,
                  Entity editorSelection);

        // Game-half serial window (EditorLayer::ProcessPendingActions): hands the view back when the panel
        // did not draw last frame, the only legal release point.
        void ProcessPendingActions(Renderer* renderer);

        // Hands the preview view back at process shutdown, where the per-frame release cannot run (it
        // needs a next frame). Called once from EditorLayer::ReleasePreviewViews in App's shutdown window
        // (render thread joined, device alive), the same serial-window contract as the per-frame release.
        // Idempotent.
        void ReleaseViewsAtShutdown(Renderer* renderer);

        // ---- Harness (-preview_camera=<entityName>) ----
        // Opens the panel on the entity named @p name as soon as it exists; retried every Draw until it
        // resolves, because the scene loads asynchronously and a lookup at parse time finds nothing.
        void HarnessSelectByName(const std::string& name) { m_harnessName = name; }
        // Pins the panel size, bypassing measurement and debounce: under -hidden the content region is
        // always <= 0, so a pack could otherwise never exercise a chosen size.
        void HarnessSetSize(std::uint32_t w, std::uint32_t h)
        { m_harnessW = w; m_harnessH = h; }
        // -preview_camera_resize=<frame>:<w>x<h>: moves the pinned size at a chosen panel frame, the only
        // way a pack can exercise a window-edge drag (under -hidden the measured branch never runs, and the
        // 200 ms debounce makes the commit frame depend on wall-clock speed). For a sole camera view the
        // resize re-fixes the WorldCull lane and recompiles the world graph once; with a second camera view
        // live it is refused, and the panel shows the refusal.
        // Returns false for a bad frame / extent and for a panel no -preview_camera opened (it pins name
        // and size; without it the measured branch runs and this pin never takes), so App can refuse the
        // run instead of measuring the un-resized case.
        bool HarnessSetResize(std::uint64_t frame, std::uint32_t w,
                              std::uint32_t h)
        {
            if (frame == 0 || w == 0 || h == 0) return false;
            if (m_harnessName.empty() || m_harnessW == 0) return false;
            m_harnessResizeFrame = frame;
            m_harnessResizeW = w; m_harnessResizeH = h;
            return true;
        }
        // -preview_camera_close_at=<frame>: closes the panel at a chosen panel frame, the only way a pack
        // can produce the window's X-button event under -hidden. Closing a holder of the WorldCull lane
        // lets another panel's refused size become reachable (the lane keeps its size until the last
        // camera view goes): preview_split_recover's subject. Refused for frame 0 and for a panel no
        // -preview_camera opened, as in HarnessSetResize.
        bool HarnessSetCloseAt(std::uint64_t frame)
        {
            if (frame == 0) return false;
            if (m_harnessName.empty() || m_harnessW == 0) return false;
            m_harnessCloseFrame = frame;
            return true;
        }
        // The entity the panel is showing, for a pack to assert on; NullEntity when nothing is selected.
        Entity SelectedEntity() const { return m_selected; }

    private:
        CameraPreviewWindow() = default;

        std::uint32_t m_viewId = ~0u;
        Entity        m_selected = NullEntity;
        bool          m_visibleThisFrame = false;
        bool          m_releaseQueued    = false;
        bool          m_syncToSelection  = false;

        // Committed size, and the debounce state behind it.
        std::uint32_t m_sizeW = 480, m_sizeH = 270;
        std::uint32_t m_pendW = 480, m_pendH = 270;
        double        m_pendAt = 0.0;
        // The last measured width; the debounce re-arms off it, not off m_pendW. They differ once something
        // adopts a width the panel's rectangle did not produce ("Match size"), and re-arming on m_pendW
        // would oscillate: measure, mismatch, re-arm, commit the measured width back 200 ms later, be
        // adopted again.
        std::uint32_t m_measuredW = 0;
        // The size the lane was last asked for, so the lane move runs on the commit edge, not every frame.
        std::uint32_t m_laneAskedW = 0, m_laneAskedH = 0;

        // How often the view re-renders, in preview frames. 1 = every frame.
        int           m_tickInterval = 1;
        // The framing: an aspect fitted to the panel's width, or a fixed render size shown scaled
        // into it (kFrames in the .cpp; 0 = 16:9, the size the harness pins measure against).
        int           m_frame = 0;

        std::string   m_harnessName;
        std::uint32_t m_harnessW = 0, m_harnessH = 0;
        // -preview_camera_resize, counted in panel frames (this window's own Draw calls), the clock
        // MaterialPreviewWidget's resize uses.
        std::uint64_t m_harnessResizeFrame = 0;
        std::uint32_t m_harnessResizeW = 0, m_harnessResizeH = 0;
        // -preview_camera_close_at, counted in the same panel frames; 0 = never closed by the harness.
        std::uint64_t m_harnessCloseFrame   = 0;
        bool          m_harnessClosedLogged = false;
        std::uint64_t m_frameTick           = 0;
        // One-shot: the panel is showing the engine's size refusal. Cleared when
        // the request is accepted again, so the next refusal says so.
        bool          m_sizeRefusedLogged   = false;
        // Not a permanent latch: a world reload clears it so the name is re-resolved against the new
        // scene. Entity ids are recycled, so a latched id would survive a reload pointing at a different
        // entity.
        bool          m_harnessResolved = false;
        // Panel frames spent resolving m_harnessName, for the deadline check in Draw. Reset with
        // m_harnessResolved on a world reload.
        std::uint32_t m_harnessTries    = 0;

        // World::Epoch() as of the last Draw. World::Clear and World::AdoptState, the two events that
        // recycle entity ids, bump it, so comparing it notices a reload without a hook in every place that
        // performs one.
        std::uint32_t m_worldEpoch     = 0;
        bool          m_worldEpochSeen = false;

        // ---- Re-resolve evidence ----
        // The panel drops its selection on every epoch bump and re-resolves by name; across an
        // ids-preserving boundary (Play -> Stop, World::AdoptState) the name must come back to the same
        // entity id. A bare "resolved to entity N" line is satisfied by the boot resolve alone, so the log
        // line also carries `resolves` (times the name was resolved in this process; 1 = never
        // re-resolved) and `same` (this resolve's id vs the first one), read from the last matching line.
        Entity        m_firstResolved   = NullEntity;
        std::uint32_t m_resolveCount    = 0;

        // ---- Panel visibility ----
        // A harness-requested panel that stops drawing produces no picture, no world-cull line and no HDR
        // slot, and without these counters that reads as a renderer bug (e.g. a docked "Camera Preview"
        // whose dockspace is skipped or degenerate gets Begin() == false every frame, with nothing in the
        // log naming the cause). Counted unconditionally (three increments a frame); only the -panelvisibleverify
        // report and its ENGINE_CHECK are gated.
        std::uint64_t m_drewFrames         = 0;  // ProcessPendingActions saw a draw
        std::uint64_t m_hiddenFrames       = 0;  // ...did not, while a name was asked for
        std::uint64_t m_beginRefusedFrames = 0;  // ImGui::Begin() itself returned false
        std::uint32_t m_autoReleases       = 0;  // releases taken by the not-visible path
    };
}
