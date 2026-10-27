#pragma once

// SplitViewportPanel - one dockable editor panel holding 2..4 secondary scene
// views, each a live WorldCull picture through a chosen scene camera, laid out
// 1x2 or 2x2 and refreshed round-robin.
//
// One panel rather than N CameraPreviewWindows: every live WorldCull view draws
// through the same m_world GraphSet, and a RenderGraph has exactly one size (a
// change destroys and recreates seven physical textures). Only an object that
// owns all N cells and divides its own rectangle can make them agree.
// Renderer::RequestPreview enforces the rule on its own (the first live view
// fixes the size, a mismatch is refused with one log line); there is
// deliberately no Renderer query to pre-check a size with (Renderer.h says why,
// beside AcquirePreviewView). A size change is one
// Renderer::RequestPreviewWorldCullLaneSize call per commit carrying EVERY live
// cell's view id: the engine only moves a fixed lane for a caller that owns all
// of it, so a per-cell ask is always refused.
//
// Cost: the cells share lane 1, so the scheduler records at most one of them per
// frame, hovered first, then oldest rendered (four cells at tickIntervalFrames 1
// each refresh every fourth preview frame). No second graph, lane or
// constant-buffer ring set.
//
// Input: hover is the only per-view editor state wired (a hovered cell is served
// first). The panel writes no main-viewport input state (m_vpMouseDX/DY,
// m_viewportHovered, m_viewportOtherItemActive), submits no picking, gizmo or
// drag-drop, and the cells are perspective only: EditorViewMode's ortho override
// lives on one App seam with two hand-synchronised fwd/up tables, and moving half
// of it gives a silently wrong view.
//
// UI invariants shared with CameraPreviewWindow:
//   1. no input capture and no main-viewport input state written;
//   2. the SRV is re-resolved every frame and never cached;
//   3. the cell size is quantised (16 px) and debounced (200 ms), because a
//      commit recompiles the graph;
//   4. per-frame state is reset at the top of every Draw rather than latched.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"                          // Entity
#include "Renderer/Preview/PreviewTypes.h"    // Preview::kMaxPreviews, ViewDesc

#include <cstdint>
#include <string>
#include <vector>

class Renderer;
class World;

namespace Editor
{
    class SplitViewportPanel
    {
    public:
        static SplitViewportPanel& Get();

        // Draws the panel; called from EditorLayer::OnUIRender behind its
        // m_showSplitViewport flag. @p open is the menu-item flag.
        void Draw(Renderer* renderer, World* world, bool* open);

        // Game-half serial window (EditorLayer::ProcessPendingActions): releases every
        // cell's view once the panel stopped drawing. This is the only legal release point.
        void ProcessPendingActions(Renderer* renderer);

        // Releases every cell's preview view at process shutdown, where the per-frame
        // release (which needs a next frame) never runs. Called once from
        // EditorLayer::ReleasePreviewViews in App's shutdown window (render thread
        // joined, device alive). Idempotent.
        void ReleaseViewsAtShutdown(Renderer* renderer);

        // ---- Harness (-preview_split=<cams>:<w>x<h>[:<tick>]) ----
        // Pins the cells to the named scene cameras, the cell size (bypassing the
        // measurement and the 200 ms debounce: under -hidden the content region is
        // always <= 0) and the tick. Returns false when @p camNames is not 2..4 entries,
        // the size does not survive the quantiser, or @p tick is not one the per-cell
        // combo offers (1 / 2 / 3 / 4 / 8 - the first click on a combo that cannot show
        // the tick would retune the run). The caller refuses the run on false.
        bool HarnessConfigure(const std::vector<std::string>& camNames,
                              std::uint32_t w, std::uint32_t h,
                              std::uint32_t tick);

        // How many cells currently hold a live view id. UI-thread only, for the panel's
        // header text; packs assert on PreviewRenderer's shutdown `[PREVIEW] split:` line
        // (`views=` the peak of concurrently live views, `rendered=` the views that ever drew).
        std::uint32_t LiveCellCount() const;

        // -preview_split_hover=<cell>: pins one cell as hovered for the whole run, the
        // only way to exercise the scheduler's hover preference under -hidden
        // (io.DisplaySize is 0, so IsItemHovered never fires). It pins the scheduler's
        // input; the non-starving rule that turns it into a double share lives in
        // PreviewRenderer::ProcessQueue. Returns false for a negative @p cell, an index
        // outside the cells -preview_split configured, or no -preview_split at all
        // (a precondition failure, exit 12).
        bool HarnessSetHoverCell(int cell);

        // One step of -preview_split_layout_at=<f>:<l>[,<f>:<l>...]: switches the layout
        // (0 = 1x2, 1 = 2x2) at panel frame @p frame. A shrink must hand the closed
        // cells' views back (CellCount() is the one authority on who is open); a
        // re-expansion must re-resolve each camera by name. Call once per step, in any
        // order; steps apply in frame order. Returns false for frame 0, a bad @p layout,
        // a repeated frame, more than kMaxLayoutSteps steps, or no -preview_split
        // (exit 12).
        bool HarnessSetLayoutAt(std::uint64_t frame, int layout);

        // One step of -preview_split_resize_at=<frame>:<w>x<h>[,<frame>:<w>x<h>...]:
        // moves the pinned cell size at panel frame @p frame - a drag-resize without the
        // wall clock, the only way a pack reaches the lane move with several live cells.
        // Call once per step, in any order; each applies once, in frame order. A repeated
        // flag is no substitute: CommandLine is last-wins. Returns false for frame 0, a
        // size the quantiser would change, a repeated frame, more than kMaxResizeSteps
        // steps, or no -preview_split.
        bool HarnessSetResizeAt(std::uint64_t frame, std::uint32_t w,
                                std::uint32_t h);

        // -preview_match_size_at=<frame>: presses "Match size" on the first refused cell
        // at panel frame @p frame. A flag, not a click: under -hidden there is no cursor,
        // and the harness pin rewrites m_cellW/m_cellH every Draw anyway. It runs the
        // button's own body (AdoptLaneSize), so the pack measures the shipped adoption.
        // Returns false for frame 0 or no -preview_split.
        bool HarnessSetMatchSizeAt(std::uint64_t frame);

    private:
        // One cell per live HDR slot: beyond Preview::kMaxPreviews AcquireView hands out
        // nothing and the cell would show "no free view" forever.
        static constexpr std::uint32_t kMaxCells = Preview::kMaxPreviews;

        struct Cell
        {
            Entity        selected = NullEntity;
            std::uint32_t viewId   = ~0u;
            int           tick     = 1;
            bool          releaseQueued = false;
            // The harness camera name, resolved by retrying until the scene has loaded (it
            // loads asynchronously). Re-armed on a world reload.
            std::string   harnessName;
            bool          harnessResolved = false;
        };

        static constexpr std::uint32_t kMaxLayoutSteps = 4;
        struct LayoutStep { std::uint64_t frame = 0; int layout = -1; bool done = false; };

        static constexpr std::uint32_t kMaxResizeSteps = 4;
        struct ResizeStep
        {
            std::uint64_t frame = 0;
            std::uint32_t w = 0, h = 0;
            bool          done = false;
        };

        // The body of the "Match size" button, shared with the harness step: every cell
        // adopts the lane's size @p lw x @p lh; @p how names the trigger in the log line.
        void AdoptLaneSize(std::uint32_t lw, std::uint32_t lh, const char* how);

        SplitViewportPanel() = default;

        std::uint32_t CellCount() const { return (m_layout == 0) ? 2u : 4u; }

        Cell m_cells[kMaxCells];

        // 0 = 1x2 (two cells side by side), 1 = 2x2 (four).
        int  m_layout = 1;

        bool m_visibleThisFrame = false;

        // Committed cell size and its debounce. All cells share it: that is the engine's
        // one-size rule, not a layout choice.
        std::uint32_t m_cellW = 320, m_cellH = 180;
        std::uint32_t m_pendW = 320, m_pendH = 180;
        double        m_pendAt = 0.0;
        // The last measured rectangle. The debounce re-arms off this, not off
        // m_pendW/H: "Match size" writes m_cellW/m_pendW without the rectangle moving,
        // and re-arming on m_pendW would commit the measured size back 200 ms later,
        // forever.
        std::uint32_t m_measuredW = 0, m_measuredH = 0;
        // The lane size last moved to successfully, so the lane move runs only when
        // the committed size differs from it.
        std::uint32_t m_laneAskedW = 0, m_laneAskedH = 0;

        std::uint32_t m_harnessW = 0, m_harnessH = 0;
        // Panel frames spent resolving the harness names; at 120 an ENGINE_CHECK reports
        // it, because retrying forever in silence is a false pass.
        std::uint32_t m_harnessTries = 0;
        bool          m_harnessArmed = false;

        // World::Epoch() as of the last Draw. World::Clear and World::AdoptState bump
        // it (the two events that recycle entity ids), which is how the panel notices a
        // reload without a hook. Entity has no generation: a selection kept across a
        // reload would follow whatever entity now holds the id.
        std::uint32_t m_worldEpoch     = 0;
        bool          m_worldEpochSeen = false;

        // One-shot: the size refusal has been logged.
        bool          m_sizeRefusedLogged = false;

        // -preview_split_hover. -1 = off (the real cursor decides).
        int           m_harnessHover       = -1;
        bool          m_harnessHoverLogged = false;
        bool          m_harnessHoverDropLogged = false;
        // How many cells -preview_split named. The hover index is validated
        // against THIS, not against the storage bound.
        std::uint32_t m_harnessCells       = 0;

        // -preview_split_layout_at steps, counted in panel frames (this panel's own Draw
        // calls). A list: shrink-then-re-expand is two steps.
        LayoutStep    m_harnessLayoutSteps[kMaxLayoutSteps]{};
        std::uint32_t m_harnessLayoutCount = 0;

        // -preview_split_resize_at steps, same clock. A list: consecutive resizes must
        // each cost one compile (compiles = 1 + N).
        ResizeStep    m_harnessResizeSteps[kMaxResizeSteps]{};
        std::uint32_t m_harnessResizeCount = 0;
        std::uint64_t m_frameTick          = 0;
        // -preview_match_size_at=<panel frame>; 0 = off. Drives the same adoption as
        // the "Match size" button, which is unreachable under -hidden.
        std::uint64_t m_harnessMatchFrame  = 0;
        bool          m_harnessMatchDone   = false;
        // Latched warning: the match step came round with no refused cell to press.
        // One WARNING, since the condition then holds on every later frame.
        bool          m_harnessMatchIdleLogged = false;
        // How many times each cell has resolved its harness camera by name; logged as
        // "resolve #N" so a re-expansion's fresh resolve is told apart from the first.
        std::uint32_t m_harnessResolves[kMaxCells] = {};
    };
}
