// SplitViewportPanel.cpp - see the header for the contract and the one-size rule.

#include "Editor/SplitViewportPanel.h"

#include "Editor/PreviewFrozenSubmit.h"  // SubmitFrozenImage / kFrozenTint
#include "ECS/CameraStackComponents.h"   // VirtualCameraComponent
#include "ECS/Components.h"              // CameraComponent
#include "ECS/ECS.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>

namespace Editor
{
namespace
{
    // A commit recompiles the graph (seven physical textures destroyed and
    // recreated), so the reachable sizes are few and the commit is debounced.
    // 16-pixel steps on both axes rather than CameraPreviewWindow's "quantise the
    // width, derive 16:9": a cell is whatever shape the panel's rectangle divided by
    // the layout is, and forcing 16:9 onto a tall dock would letterbox most of it.
    constexpr double kResizeDebounceSeconds = 0.2;
    constexpr std::uint32_t kMinW = 160, kMaxW = 960;
    constexpr std::uint32_t kMinH =  96, kMaxH = 540;

    // A preview camera and its combo label.
    struct CamRow { Entity e; std::string label; };

    // The per-cell tick combo's entries. HarnessConfigure validates -preview_split's
    // tick against this set, so a harness tick always displays as itself and the
    // first click on the combo cannot silently retune the run.
    const char* const kTickLabels[] = { "1", "2", "3", "4", "8" };
    const int         kTickValues[] = {  1,   2,   3,   4,   8  };
    constexpr int     kTickCount    = 5;

    // The layout's name for the log (the combo's labels are spaced for the eye).
    // Indexed by SplitViewportPanel::m_layout: keep the order.
    const char* const kLayoutNames[] = { "1x2", "2x2" };

    std::uint32_t Quantise(float avail, std::uint32_t lo, std::uint32_t hi)
    {
        std::uint32_t v = static_cast<std::uint32_t>(std::max(avail, 0.f));
        v = (v / 16u) * 16u;
        return std::clamp(v, lo, hi);
    }

    // Every entity that can serve as a preview camera, with its display name. Both
    // pools, because a VCam rig carries VirtualCameraComponent and need not carry
    // CameraComponent; de-duplicated because a bridged rig carries both.
    void GatherCameras(World& world, std::vector<CamRow>& out)
    {
        out.clear();
        auto add = [&](Entity e, const char* kind)
        {
            for (const CamRow& r : out) if (r.e == e) return;
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s  [%s #%u]",
                          world.GetName(e).c_str(), kind,
                          static_cast<unsigned>(e));
            out.push_back({ e, buf });
        };
        world.ForEach<CameraComponent>(
            [&](Entity e, CameraComponent&) { add(e, "Camera"); });
        world.ForEach<VirtualCameraComponent>(
            [&](Entity e, VirtualCameraComponent&) { add(e, "VCam"); });
    }

    bool IsOfferedTick(std::uint32_t tick)
    {
        for (int k = 0; k < kTickCount; ++k)
            if (kTickValues[k] == static_cast<int>(tick)) return true;
        return false;
    }
}

SplitViewportPanel& SplitViewportPanel::Get()
{
    static SplitViewportPanel s_instance;
    return s_instance;
}

bool SplitViewportPanel::HarnessConfigure(const std::vector<std::string>& camNames,
                                          std::uint32_t w, std::uint32_t h,
                                          std::uint32_t tick)
{
    // 2..4 is the feature's bound: one cell is CameraPreviewWindow, and a fifth
    // would have no HDR slot (Preview::kMaxPreviews).
    if (camNames.size() < 2 || camNames.size() > kMaxCells) return false;
    if (w == 0 || h == 0) return false;
    // The size must survive the quantiser unchanged, or the pack would run at a
    // size nobody asked for. Same rule as -preview_asset's quantised-size refusal.
    if (Quantise(static_cast<float>(w), kMinW, kMaxW) != w) return false;
    if (Quantise(static_cast<float>(h), kMinH, kMaxH) != h) return false;
    // Against the combo's own set, not 1..8: a tick the combo cannot display would
    // be changed by the first click.
    if (!IsOfferedTick(tick)) return false;

    m_layout = (camNames.size() <= 2) ? 0 : 1;
    for (std::uint32_t i = 0; i < kMaxCells; ++i)
    {
        m_cells[i] = Cell{};
        m_harnessResolves[i] = 0;
        if (i < camNames.size())
        {
            m_cells[i].harnessName = camNames[i];
            m_cells[i].tick        = static_cast<int>(tick);
        }
    }
    m_harnessW = m_cellW = m_pendW = w;
    m_harnessH = m_cellH = m_pendH = h;
    m_harnessArmed = true;
    m_harnessCells = static_cast<std::uint32_t>(camNames.size());
    m_harnessTries = 0;
    LOG_INFO("[PREVIEW] split: harness armed with %zu camera(s) at %ux%u tick %u "
             "(layout %s)", camNames.size(), w, h, tick,
             (m_layout == 0) ? "1x2" : "2x2");
    return true;
}

bool SplitViewportPanel::HarnessSetHoverCell(int cell)
{
    // Against the configured cells, not the storage bound. Also false when no
    // -preview_split configured any cells, so the hover flag alone is a
    // precondition failure rather than a run measuring the unhovered cadence.
    if (cell < 0 || !m_harnessArmed) return false;
    if (static_cast<std::uint32_t>(cell) >= m_harnessCells) return false;
    m_harnessHover = cell;
    return true;
}

bool SplitViewportPanel::HarnessSetLayoutAt(std::uint64_t frame, int layout)
{
    // Refused when no -preview_split opened the panel, like the hover flag:
    // "accepted and did nothing" would measure the unswitched layout under the
    // switch pack's name.
    if (frame == 0 || layout < 0 || layout > 1 || !m_harnessArmed) return false;
    if (m_harnessLayoutCount >= kMaxLayoutSteps) return false;
    // No two steps on one frame: both would apply on one Draw and the second would
    // hide the first.
    for (std::uint32_t i = 0; i < m_harnessLayoutCount; ++i)
        if (m_harnessLayoutSteps[i].frame == frame) return false;
    m_harnessLayoutSteps[m_harnessLayoutCount].frame  = frame;
    m_harnessLayoutSteps[m_harnessLayoutCount].layout = layout;
    m_harnessLayoutSteps[m_harnessLayoutCount].done   = false;
    ++m_harnessLayoutCount;
    return true;
}

bool SplitViewportPanel::HarnessSetResizeAt(std::uint64_t frame,
                                            std::uint32_t w, std::uint32_t h)
{
    // Same rules as every flag naming this panel: a real frame, a size the
    // quantiser leaves alone, and a panel -preview_split opened.
    if (frame == 0 || w == 0 || h == 0 || !m_harnessArmed) return false;
    if (Quantise(static_cast<float>(w), kMinW, kMaxW) != w) return false;
    if (Quantise(static_cast<float>(h), kMinH, kMaxH) != h) return false;
    // One step per call, and no two steps on one frame (the layout list's rule):
    // both would apply on one Draw and the second would hide the first.
    if (m_harnessResizeCount >= kMaxResizeSteps) return false;
    for (std::uint32_t i = 0; i < m_harnessResizeCount; ++i)
        if (m_harnessResizeSteps[i].frame == frame) return false;
    ResizeStep& st = m_harnessResizeSteps[m_harnessResizeCount++];
    st.frame = frame;
    st.w     = w;
    st.h     = h;
    st.done  = false;
    return true;
}

bool SplitViewportPanel::HarnessSetMatchSizeAt(std::uint64_t frame)
{
    // Same two rules every flag naming this panel has.
    if (frame == 0 || !m_harnessArmed) return false;
    m_harnessMatchFrame = frame;
    m_harnessMatchDone  = false;
    return true;
}

void SplitViewportPanel::AdoptLaneSize(std::uint32_t lw, std::uint32_t lh,
                                       const char* how)
{
    // m_measuredW/H are deliberately not touched: they hold the last size the
    // panel's rectangle produced, and the debounce re-arms on a change to that. An
    // unchanged rectangle re-measures to the same numbers, so the adoption sticks;
    // writing lw/lh into them would make the debounce commit the measured size back
    // 200 ms later, forever.
    m_cellW = m_pendW = lw;
    m_cellH = m_pendH = lh;
    // The pin moves with it: the pin block at the top of Draw rewrites
    // m_cellW/m_cellH from m_harnessW/H every frame, so an adoption that left the
    // pin behind would be undone at once.
    if (m_harnessW != 0) { m_harnessW = lw; m_harnessH = lh; }
    // One line, so a pack can gate on the adoption rather than infer it.
    LOG_INFO("[PREVIEW] split: adopted the lane size %ux%u (%s) - every cell "
             "now asks for the size the lane already has", lw, lh, how);
}

std::uint32_t SplitViewportPanel::LiveCellCount() const
{
    std::uint32_t n = 0;
    for (const Cell& c : m_cells) if (c.viewId != ~0u) ++n;
    return n;
}

void SplitViewportPanel::Draw(Renderer* renderer, World* world, bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(760.f, 480.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Split Viewports", open))
    {
        // Collapsed is not visible: m_visibleThisFrame stays false, so the next
        // ProcessPendingActions releases every cell's view rather than letting a folded
        // tab hold four HDR slots.
        ImGui::End();
        return;
    }

    if (!renderer || !world)
    {
        ImGui::TextDisabled("No renderer / world attached.");
        ImGui::End();
        return;
    }
    if (!renderer->IsPreviewEnabled())
    {
        // The preview kill switch, reported rather than shown as four blank boxes; this
        // is also what turns the split-viewport packs red under DX12_PREVIEW=0.
        ImGui::TextDisabled("Preview disabled (DX12_PREVIEW=0 or "
                            "editor_prefs preview.enabled=0)");
        ImGui::End();
        return;
    }

    m_visibleThisFrame = true;

    // ---- A world reload invalidates every selection ----
    // Entity is a bare uint32 id with no generation, so a selection carried across a
    // Load or a Play->Stop restore would silently follow whatever entity now holds
    // the id. World::Epoch() is bumped by Clear and AdoptState, the two events that
    // recycle ids. The harness resolve is re-armed too: a latched "resolved" would
    // keep the cell on a dead id while reporting success.
    const std::uint32_t epoch = world->Epoch();
    if (epoch != m_worldEpoch)
    {
        if (m_worldEpochSeen)
            LOG_INFO("[PREVIEW] split: world epoch %u -> %u, dropping every cell's "
                     "camera selection (entity ids are recycled across a reload)",
                     m_worldEpoch, epoch);
        m_worldEpoch     = epoch;
        m_worldEpochSeen = true;
        for (Cell& c : m_cells)
        {
            c.selected        = NullEntity;
            c.harnessResolved = false;
        }
        m_harnessTries = 0;
    }

    // ---- -preview_split_layout_at steps ----
    // Applied at the top of Draw, so the count below is already the new one and the
    // release loop after it sees the cells this frame closed. Each step fires once
    // (`done`): "apply whenever the layout differs" would make an earlier step fight
    // a later one.
    for (std::uint32_t s = 0; s < m_harnessLayoutCount; ++s)
    {
        LayoutStep& st = m_harnessLayoutSteps[s];
        if (st.done || st.layout < 0 || m_frameTick < st.frame) continue;
        st.done = true;
        if (m_layout == st.layout) continue;
        LOG_INFO("[PREVIEW] split: layout %s -> %s at panel frame %llu (harness)",
                 kLayoutNames[m_layout], kLayoutNames[st.layout],
                 static_cast<unsigned long long>(m_frameTick));
        m_layout = st.layout;
    }
    // ---- -preview_split_resize_at steps ----
    // Moves the pin below, where the commit and the lane move happen. Each step
    // fires once (`done`): a "pin differs from the target" test would re-fire step 1
    // after step 2 moved the pin, and a step asking for the size the pin already has
    // is spent rather than re-firing forever.
    for (std::uint32_t r = 0; r < m_harnessResizeCount; ++r)
    {
        ResizeStep& st = m_harnessResizeSteps[r];
        if (st.done || m_harnessW == 0 || m_frameTick < st.frame) continue;
        st.done = true;
        if (m_harnessW == st.w && m_harnessH == st.h) continue;
        LOG_INFO("[PREVIEW] split: harness size -> %ux%u (panel frame %llu)",
                 st.w, st.h, static_cast<unsigned long long>(m_frameTick));
        m_harnessW = st.w;
        m_harnessH = st.h;
    }
    // This Draw's tick, taken before the counter moves on. The layout and resize
    // loops above read m_frameTick before the increment and the match-size step below
    // reads drawTick, so steps written with the same frame number fire on the same draw.
    const std::uint64_t drawTick = m_frameTick;
    ++m_frameTick;

    std::vector<CamRow> cams;
    GatherCameras(*world, cams);

    // ---- Layout + the shared cell size ----
    ImGui::SetNextItemWidth(120.f);
    const char* const kLayouts[] = { "1 x 2", "2 x 2" };
    if (m_harnessArmed)
        ImGui::TextDisabled("Layout: %s (harness)", kLayouts[m_layout]);
    else
        ImGui::Combo("Layout", &m_layout, kLayouts, 2);
    ImGui::SameLine();
    {
        std::uint32_t lw = 0, lh = 0;
        renderer->GetPreviewWorldCullSize(lw, lh);
        ImGui::TextDisabled("|  cells %u x %u  |  lane %u x %u  |  %u live",
                            m_cellW, m_cellH, lw, lh, LiveCellCount());
    }
    ImGui::TextDisabled("Perspective only. No picking, no gizmo, no drag-drop, "
                        "no ortho - those stay in the main viewport.");
    ImGui::Separator();

    const std::uint32_t cells = CellCount();
    const std::uint32_t cols  = 2;
    const std::uint32_t rows  = (cells + cols - 1) / cols;

    // ---- CellCount() is the only authority on who is open ----
    // Cells at or above the count are released here, right after the count is known
    // and before anything reads it, so a layout shrink is the same teardown as a
    // close. The draw loop never visits them, so otherwise they would keep a view id
    // (an HDR slot and a round-robin turn) for the rest of the session.
    for (std::uint32_t i = cells; i < kMaxCells; ++i)
    {
        Cell& c = m_cells[i];
        if (c.viewId == ~0u && c.selected == NullEntity) continue;
        if (c.viewId != ~0u)
        {
            c.releaseQueued = true;
            LOG_INFO("[PREVIEW] split: layout is %s now, so cell %u is closed - "
                     "queueing its view %u for release", kLayoutNames[m_layout],
                     i, c.viewId);
        }
        // The selection goes too: a hidden cell that kept its camera would silently
        // re-acquire a view the moment the layout grew again.
        c.selected        = NullEntity;
        c.harnessResolved = false;
    }

    if (m_harnessW != 0 && m_harnessH != 0)
    {
        // Pinned, bypassing the measurement and the debounce: under -hidden the content
        // region is always <= 0, and a debounce makes the commit frame depend on
        // wall-clock speed while a pack asserts on a frame number.
        m_cellW = m_pendW = m_harnessW;
        m_cellH = m_pendH = m_harnessH;
    }
    else
    {
        // The panel's rectangle divided by the layout, then quantised, then debounced.
        // ~26 px per cell goes to its header row (camera combo and tick); the rest is
        // the picture.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float  pad   = ImGui::GetStyle().ItemSpacing.x;
        const float  headerH = ImGui::GetFrameHeightWithSpacing() + 4.f;
        const float  cw = (avail.x - pad * static_cast<float>(cols - 1))
                        / static_cast<float>(cols);
        const float  ch = (avail.y - pad * static_cast<float>(rows - 1))
                        / static_cast<float>(rows) - headerH;
        const std::uint32_t wantW = Quantise(cw > 0.f ? cw : 320.f, kMinW, kMaxW);
        const std::uint32_t wantH = Quantise(ch > 0.f ? ch : 180.f, kMinH, kMaxH);
        const double now = ImGui::GetTime();
        // The debounce re-arms off the measured rectangle, not off the pending size:
        // after "Match size" adopts a size the rectangle did not produce, keying on
        // m_pendW would commit the measured size back 200 ms later and oscillate forever.
        // An unmoved rectangle re-triggers nothing.
        if (wantW != m_measuredW || wantH != m_measuredH)
        {
            m_measuredW = wantW; m_measuredH = wantH;
            m_pendW = wantW; m_pendH = wantH; m_pendAt = now;
        }
        else if ((m_pendW != m_cellW || m_pendH != m_cellH) &&
                 (now - m_pendAt) >= kResizeDebounceSeconds)
        { m_cellW = m_pendW; m_cellH = m_pendH; }
    }

    // ---- One lane move per commit, for the whole panel ----
    // A cell cannot move the lane on its own: the engine only lets a caller that owns
    // every live camera view move a fixed size
    // (PreviewRenderer::RequestWorldCullLaneSize), and one cell of two or more never
    // does. Asked only while the committed size differs from the last successful
    // move. The per-cell requests below still run every frame and carry the per-view
    // refusal flag, so a cross-panel conflict shows on the cell either way.
    {
        // Every cell that still holds a view, not only the ones this layout draws: a
        // shrink marks closed cells `releaseQueued` but keeps their view id until
        // ProcessPendingActions at the next frame boundary, so on the shrink frame those
        // views still count as live on the lane (the engine's m_worldCullLive). Naming
        // fewer owners would get a same-frame size commit refused.
        std::uint32_t live[kMaxCells];
        std::uint32_t liveCount = 0;
        for (std::uint32_t i = 0; i < kMaxCells; ++i)
            if (m_cells[i].viewId != ~0u) live[liveCount++] = m_cells[i].viewId;
        // Skipped while no cell holds a view (the first frames, before any acquire): with
        // no owners the engine answers true while nothing is fixed yet, and latching that
        // would leave the real move unasked once the cells are live.
        if (liveCount != 0 &&
            (m_cellW != m_laneAskedW || m_cellH != m_laneAskedH))
        {
            // The latch moves on success only. The lane's size is freed only when the last
            // camera view goes, so a refused size becomes reachable once the conflicting
            // holder closes, and it must be asked again then. Re-asking every frame while
            // refused is cheap: the call early-outs on a size already fixed, and the engine
            // logs each refused size once.
            if (renderer->RequestPreviewWorldCullLaneSize(m_cellW, m_cellH,
                                                          live, liveCount))
            {
                m_laneAskedW = m_cellW;
                m_laneAskedH = m_cellH;
            }
        }
    }

    // ---- A request's size is not pre-checked here ----
    // Every cell asks for itself in RequestPreview, and a refused cell says so on the
    // cell: it shows the last picture it drew, greyed, at that picture's own size,
    // and offers to adopt the lane. A refused cell keeps its view id and (if it ever
    // rendered) its HDR slot and tone-map output, but the engine suspends it: no
    // round-robin turn, no new generation.

    // ---- The harness's name resolve, retried until the scene loads ----
    if (m_harnessArmed)
    {
        // A conjunction over every unresolved cell: one cell resolving must not hide
        // another that never does, or the deadline below would never arm.
        bool allResolved = true;
        for (std::uint32_t i = 0; i < cells; ++i)
        {
            Cell& c = m_cells[i];
            if (c.harnessName.empty() || c.harnessResolved) continue;
            bool found = false;
            for (const CamRow& r : cams)
                if (world->GetName(r.e) == c.harnessName)
                {
                    c.selected        = r.e;
                    c.harnessResolved = true;
                    found             = true;
                    // The resolve count is on the line: a re-expansion resolves the same name
                    // again, and only the count tells that resolve from the first.
                    ++m_harnessResolves[i];
                    LOG_INFO("[PREVIEW] split: cell %u -> '%s' (entity %u, "
                             "resolve #%u)",
                             i, c.harnessName.c_str(),
                             static_cast<unsigned>(r.e), m_harnessResolves[i]);
                    break;
                }
            if (!found) allResolved = false;
        }
        // Retrying forever in silence is a false pass: a name that never matches leaves
        // its cell empty. Counted in panel draws (what this class can see); 120 is ~2 s
        // at 60 Hz and every preview pack runs 200+ frames. Consecutive, not cumulative:
        // cleared on any frame every open cell is resolved, so a re-expansion does not
        // inherit the frames the scene load spent.
        if (allResolved) m_harnessTries = 0;
        else if (++m_harnessTries == 120)
        {
            std::string missing;
            for (std::uint32_t i = 0; i < cells; ++i)
                if (!m_cells[i].harnessName.empty() && !m_cells[i].harnessResolved)
                { missing += m_cells[i].harnessName; missing += ' '; }
            ENGINE_CHECK_ONCE(false,
                "-preview_split: the camera name(s) '%s' never resolved in 120 "
                "panel frames. Those cells are showing nothing and this run is "
                "measuring fewer views than the pack's name claims. Check the "
                "entity names (percent-encode spaces) and that the scene "
                "contains them.", missing.c_str());
        }
    }

    // ---- The cells ----
    const float  headerH = ImGui::GetFrameHeightWithSpacing() + 4.f;
    const ImVec2 cellBox(static_cast<float>(m_cellW) + 8.f,
                         static_cast<float>(m_cellH) + headerH + 8.f);
    std::uint32_t hovered = Preview::kInvalidView;
    bool anyRefused = false;

    for (std::uint32_t i = 0; i < cells; ++i)
    {
        Cell& c = m_cells[i];
        if ((i % cols) != 0) ImGui::SameLine();

        char childId[32];
        std::snprintf(childId, sizeof(childId), "##splitcell%u", i);
        // NoScrollbar / NoScrollWithMouse: invariant 1. A scrolling child would take the
        // wheel from whatever is behind it, and the panel has no camera of its own.
        ImGui::BeginChild(childId, cellBox, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

        // ---- Cell header: camera + tick ----
        const char* preview = "(none)";
        for (const CamRow& r : cams) if (r.e == c.selected) preview = r.label.c_str();
        ImGui::SetNextItemWidth(static_cast<float>(m_cellW) * 0.62f);
        char comboId[32];
        std::snprintf(comboId, sizeof(comboId), "##cam%u", i);
        if (ImGui::BeginCombo(comboId, preview))
        {
            if (ImGui::Selectable("(none)", c.selected == NullEntity))
                c.selected = NullEntity;
            for (const CamRow& r : cams)
                if (ImGui::Selectable(r.label.c_str(), r.e == c.selected))
                {
                    c.selected = r.e;
                    // A hand pick overrides the harness for this cell; marking it resolved stops
                    // the retry loop putting the harness camera back next frame.
                    c.harnessResolved = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        int tickIdx = 0;
        for (int k = 0; k < kTickCount; ++k)
            if (kTickValues[k] == c.tick) tickIdx = k;
        ImGui::SetNextItemWidth(64.f);
        char tickId[32];
        std::snprintf(tickId, sizeof(tickId), "1/N##tick%u", i);
        if (ImGui::Combo(tickId, &tickIdx, kTickLabels, kTickCount))
            c.tick = kTickValues[std::clamp(tickIdx, 0, kTickCount - 1)];

        // ---- The picture ----
        const ImVec2 imgSize(static_cast<float>(m_cellW),
                             static_cast<float>(m_cellH));
        if (c.selected == NullEntity)
        {
            // No camera, no view: the slot is handed back rather than held on a
            // picture nothing updates.
            if (c.viewId != ~0u) c.releaseQueued = true;
            ImGui::Dummy(imgSize);
            ImGui::TextDisabled("Select a camera.");
            ImGui::EndChild();
            continue;
        }
        if (c.releaseQueued)
        {
            ImGui::Dummy(imgSize);
            ImGui::TextDisabled("switching...");
            ImGui::EndChild();
            continue;
        }
        if (!world->IsAlive(c.selected))
        {
            c.selected = NullEntity;
            ImGui::Dummy(imgSize);
            ImGui::TextDisabled("Camera entity is gone.");
            ImGui::EndChild();
            continue;
        }

        if (c.viewId == ~0u)
        {
            c.viewId = renderer->AcquirePreviewView(/*worldCull=*/true);
            if (c.viewId == ~0u)
            {
                ImGui::Dummy(imgSize);
                ImGui::TextDisabled("No free preview view (max %u).",
                                    Preview::kMaxPreviews);
                ImGui::EndChild();
                continue;
            }
        }

        // The entity is pushed every frame (one store); Renderer::ExtractPreviewCameras
        // reads it on the game half. Latching on change would leave the resolver on a
        // deleted entity across a reload that reused the id.
        renderer->SetPreviewCameraEntity(c.viewId, c.selected);

        Preview::ViewDesc desc{};
        desc.w      = m_cellW;
        desc.h      = m_cellH;
        desc.source = Preview::Source::WorldCull;
        desc.tickIntervalFrames = static_cast<std::uint32_t>(c.tick);
        // The scene's own sky behind the geometry, which is what makes a cell
        // look like the game rather than like a cut-out on the clear colour.
        desc.features   = Preview::Feature::kSkybox;
        desc.background = Preview::Background::Skybox;
        // Meaningless for a WorldCull view (the scene's own material table is
        // bound), so a default component rather than a fabricated one.
        renderer->RequestPreview(c.viewId, desc, MaterialComponent{});

        // ---- Size refused: the last picture, on the cell ----
        // The engine suspends a refused view (no turn, no generation); what it still owns
        // is the last picture it drew. Show that, greyed, at its own size, with a label:
        // stretching it into the rect the cell now wants would present a stale frame as
        // live at a size it never had, and showing nothing throws away the only thing the
        // user can read while deciding whether to match the lane.
        if (renderer->IsPreviewViewSizeRefused(c.viewId))
        {
            std::uint32_t lw = 0, lh = 0;
            renderer->GetPreviewWorldCullSize(lw, lh);
            std::uint32_t ow = 0, oh = 0;
            const bool haveLast = renderer->GetPreviewOutputSize(c.viewId, ow, oh);
            const std::uint64_t lastSrv =
                haveLast ? renderer->GetPreviewSrv(c.viewId) : 0ull;
            anyRefused = true;
            if (!m_sizeRefusedLogged)
            {
                m_sizeRefusedLogged = true;
                // preview_split_size asserts this literal up to the comma: keep it verbatim. The
                // tail says which refused state the cell is in (frozen on a real frame, or never
                // drew anything).
                LOG_INFO("[PREVIEW] split: panel is showing the size refusal "
                         "(wanted %ux%u, lane is %ux%u) - %s",
                         m_cellW, m_cellH, lw, lh,
                         lastSrv ? "the cell is showing its LAST picture, frozen"
                                 : "the cell has no picture to show yet");
            }
            if (lastSrv)
            {
                // Greyed by a tint, not only a caption: the one place in the editor where a real
                // scene picture is deliberately not live must look unlike the live cells beside
                // it. SubmitFrozenImage submits the image and logs the rect it passed in one
                // expression. `ow/oh` come from Renderer::GetPreviewOutputSize, independent of
                // m_cellW/m_cellH, so the pack's "submit != cell, out == lane" pair is a
                // measurement, not arithmetic; under -hidden that log line is the whole
                // observable.
                SubmitFrozenImage("[PREVIEW] split:", c.viewId,
                                  ImTextureRef(static_cast<ImTextureID>(lastSrv)),
                                  ow, oh, m_cellW, m_cellH, lw, lh);
                ImGui::TextDisabled("FROZEN %u x %u - size refused: the lane is "
                                    "%u x %u, this cell wants %u x %u",
                                    ow, oh, lw, lh, m_cellW, m_cellH);
            }
            else
            {
                ImGui::Dummy(imgSize);
                ImGui::TextDisabled("size refused: the lane is %u x %u, this cell "
                                    "wants %u x %u", lw, lh, m_cellW, m_cellH);
            }
            ImGui::TextDisabled("Every camera view shares ONE render graph, "
                                "which has ONE size.");
            // The button and the harness step run the same body. The step is one-shot and
            // fires on the first refused cell it reaches - the one a user would click; every
            // cell shares the pin, so one adoption covers all of them.
            const bool clicked =
                (lw != 0 && lh != 0 && ImGui::SmallButton("Match size"));
            const bool harnessMatch =
                (lw != 0 && lh != 0 && m_harnessMatchFrame != 0 &&
                 !m_harnessMatchDone && drawTick >= m_harnessMatchFrame);
            if (harnessMatch) m_harnessMatchDone = true;
            if (clicked || harnessMatch)
                AdoptLaneSize(lw, lh, harnessMatch ? "harness" : "Match size");
            ImGui::EndChild();
            continue;
        }

        // Invariant 2: resolved here, every frame. Descriptor slots recycle through a
        // free list, and ImGui's own SRVs come from the same heap.
        const std::uint64_t srv = renderer->GetPreviewSrv(c.viewId);
        if (srv)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), imgSize);
            // The one per-view seam: hover. IsItemHovered names the last submitted item, so
            // this must sit right after the Image; it re-orders the scheduler and writes no
            // main-viewport input state.
            if (ImGui::IsItemHovered())
            {
                hovered = c.viewId;
                ImGui::SetTooltip("Cell %u - view %u, 1/%d frames.\nSecondary "
                                  "view: no point/spot lights, shadows borrowed "
                                  "from the main camera's cascades, no water, no "
                                  "post-processing. Hovering serves this cell "
                                  "first in the round-robin.",
                                  i, c.viewId, c.tick);
            }
        }
        else
        {
            ImGui::Dummy(imgSize);
            ImGui::TextDisabled("Rendering...");
        }
        ImGui::EndChild();
    }

    // No cell is refused any more: re-arm the one-shot so the NEXT refusal (a
    // second panel opened, a resize into a shared lane) says so too.
    if (!anyRefused) m_sizeRefusedLogged = false;

    // ---- A match-size step that found nothing to press says so ----
    // The step presses the first refused cell's button, which exists only inside the
    // refusal branch, so with no refused cell it does nothing and a pack would
    // measure the plain accepted-size path under the adoption's name. The step stays
    // armed for a later refusal; only the warning is latched, since the condition
    // then holds every frame.
    if (m_harnessMatchFrame != 0 && !m_harnessMatchDone && !anyRefused &&
        drawTick >= m_harnessMatchFrame && !m_harnessMatchIdleLogged)
    {
        m_harnessMatchIdleLogged = true;
        LOG_WARNING("[PREVIEW] split: -preview_match_size_at=%llu came round at "
                    "panel frame %llu with NO REFUSED CELL - there was no "
                    "\"Match size\" button to press, so the step has done "
                    "nothing. It stays armed for a later refusal. A pack that "
                    "asserts on the adoption is measuring the plain accepted-"
                    "size path under that step's name.",
                    static_cast<unsigned long long>(m_harnessMatchFrame),
                    static_cast<unsigned long long>(drawTick));
    }

    // -preview_split_hover: pin the preference, because under -hidden
    // io.DisplaySize is 0 and IsItemHovered() can never fire, so the scheduler's
    // one per-view seam would be dead code in every pack.
    if (m_harnessHover >= 0 && static_cast<std::uint32_t>(m_harnessHover) < cells)
    {
        hovered = m_cells[m_harnessHover].viewId;
        if (!m_harnessHoverLogged && hovered != ~0u)
        {
            m_harnessHoverLogged = true;
            LOG_INFO("[PREVIEW] split: cell %d pinned HOVERED (view %u) - it is "
                     "preferred by the scheduler on every frame it did not "
                     "already render", m_harnessHover, hovered);
        }
    }
    else if (m_harnessHover >= 0 && !m_harnessHoverDropLogged)
    {
        // Said out loud: the caller validates the index against the configured cell
        // count, so this is the second line of defence (a layout that shrank under a
        // pinned hover). A silent drop would measure the unhovered cadence under the
        // hover pack's name.
        m_harnessHoverDropLogged = true;
        LOG_WARNING("[PREVIEW] split: pinned hover cell %d is outside the %u cell"
                    "(s) this layout opens; the preference is DROPPED and the "
                    "scheduler is running its plain round-robin.",
                    m_harnessHover, cells);
    }

    // Pushed every frame, including the "nothing hovered" answer: a latched
    // preference would keep serving a cell the cursor left, which is the same
    // starvation the round-robin exists to remove.
    renderer->SetPreviewPriorityView(hovered);

    ImGui::End();
}

void SplitViewportPanel::ProcessPendingActions(Renderer* renderer)
{
    // Auto-release. The panel can stop drawing without reaching a Close button
    // (menu item unticked, window collapsed, F11 hides every panel), so it is
    // detected here rather than notified. This runs before this frame's Draw, so the
    // flag holds the previous frame's answer: "the panel did not draw last frame".
    if (!m_visibleThisFrame)
        for (Cell& c : m_cells) if (c.viewId != ~0u) c.releaseQueued = true;
    m_visibleThisFrame = false;

    for (std::uint32_t i = 0; i < kMaxCells; ++i)
    {
        Cell& c = m_cells[i];
        if (!c.releaseQueued) continue;
        c.releaseQueued = false;
        if (c.viewId != ~0u && renderer)
        {
            renderer->SetPreviewCameraEntity(c.viewId, NullEntity);
            renderer->ReleasePreviewView(c.viewId);
            // `view=` is a field so a pack keys the line on the cell and reads the view id
            // as a bounded number: which slot a cell gets back is an allocator detail.
            LOG_INFO("[PREVIEW] split: cell %u released view=%u at the frame "
                     "boundary", i, c.viewId);
        }
        c.viewId = ~0u;
    }
}

// Shutdown release (see the header): the frame-boundary release without the
// "did the panel draw" check, through the same ReleaseView.
void SplitViewportPanel::ReleaseViewsAtShutdown(Renderer* renderer)
{
    m_visibleThisFrame = false;
    for (std::uint32_t i = 0; i < kMaxCells; ++i)
    {
        Cell& c = m_cells[i];
        c.releaseQueued = false;
        if (c.viewId == ~0u) continue;
        if (renderer)
        {
            renderer->SetPreviewCameraEntity(c.viewId, NullEntity);
            renderer->ReleasePreviewView(c.viewId);
            LOG_INFO("[PREVIEW] split: cell %u released view=%u at shutdown",
                     i, c.viewId);
        }
        c.viewId = ~0u;
    }
}

} // namespace Editor
