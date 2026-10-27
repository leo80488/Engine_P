#include "Editor/ColorWidgets.h"
#include "Editor/NotifyTrackEditor.h"
#include "Editor/NotifyEditorRegistry.h"
#include "Editor/ImGuiWidgets.h"

#include "ECS/AnimationComponents.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace Editor
{

// ==== Local constants ====
// Category labels and default colours; the order MUST match NotifyCategory (indexed
// directly).
namespace
{
    constexpr const char* kCategoryNames[] = {
        "Hitbox", "VFX", "Camera", "Audio", "StateToggle", "Custom", "FXGate", "Sync", "IKGate",
    };
    static_assert(IM_ARRAYSIZE(kCategoryNames) == static_cast<int>(NotifyCategory::COUNT),
                  "kCategoryNames out of sync with NotifyCategory enum");

    constexpr ImU32 kCategoryColors[] = {
        IM_COL32(220,  60,  60, 255),  // Hitbox - red
        IM_COL32(200, 130, 255, 255),  // VFX - violet
        IM_COL32(255, 200,  60, 255),  // Camera - yellow
        IM_COL32( 80, 200, 255, 255),  // Audio - cyan
        IM_COL32(120, 220, 120, 255),  // State - green
        IM_COL32(200, 200, 200, 255),  // Custom - gray
        IM_COL32(255, 140,  60, 255),  // FXGate - orange
        IM_COL32(80, 255, 190, 255), // Sync - mint
        IM_COL32(80, 220, 160, 255),   // IKGate
    };
    static_assert(IM_ARRAYSIZE(kCategoryColors) == static_cast<int>(NotifyCategory::COUNT),
                  "kCategoryColors out of sync with NotifyCategory enum");

    // Clamp and frame-snap a time value during a drag.
    float SnapTime(float t, float clipDur, bool shiftHeld)
    {
        t = std::clamp(t, 0.f, clipDur);
        if (shiftHeld) {
            constexpr float frameRate = 60.f;
            t = std::round(t * frameRate) / frameRate;
        }
        return t;
    }
}

// ==== Concrete commands ====
// Commands store ids (stable), not pointers (invalidated by vector ops), and re-find
// their target via FindNotifyById / FindNotifyStateById on each Apply / Revert, so a
// reorder or insertion by another command does not break later replays.

namespace
{
    template <typename TFind>
    auto* FindByIdIn(TimelineComponent& tl, uint32_t id, TFind&& accessor)
    {
        for (auto& tr : tl.tracks) {
            for (auto& item : accessor(tr)) {
                if (item.id == id) return &item;
            }
        }
        return decltype(&accessor(tl.tracks[0])[0]){ nullptr };
    }
}

struct MoveNotifyCmd : INotifyCommand
{
    uint32_t id;
    float    oldT;
    float    newT;

    void Apply  (TimelineComponent& tl) override {
        if (auto* n = FindByIdIn(tl, id, [](NotifyTrack& tr) -> std::vector<Notify>& { return tr.notifies; })) n->time = newT;
    }
    void Revert (TimelineComponent& tl) override {
        if (auto* n = FindByIdIn(tl, id, [](NotifyTrack& tr) -> std::vector<Notify>& { return tr.notifies; })) n->time = oldT;
    }
    const char* Name() const override { return "Move Notify"; }
};

struct MoveStateCmd : INotifyCommand
{
    uint32_t id;
    float    oldStart, oldEnd;
    float    newStart, newEnd;

    void Apply  (TimelineComponent& tl) override {
        if (auto* s = FindByIdIn(tl, id, [](NotifyTrack& tr) -> std::vector<NotifyState>& { return tr.states; })) { s->startTime = newStart; s->endTime = newEnd; }
    }
    void Revert (TimelineComponent& tl) override {
        if (auto* s = FindByIdIn(tl, id, [](NotifyTrack& tr) -> std::vector<NotifyState>& { return tr.states; })) { s->startTime = oldStart; s->endTime = oldEnd; }
    }
    const char* Name() const override { return "Move State"; }
};

struct AddNotifyCmd : INotifyCommand
{
    int    trackIdx;
    Notify payload;

    void Apply (TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        tl.tracks[trackIdx].notifies.push_back(payload);
        std::sort(tl.tracks[trackIdx].notifies.begin(),
                  tl.tracks[trackIdx].notifies.end(),
                  [](const Notify& a, const Notify& b) { return a.time < b.time; });
    }
    void Revert(TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        auto& v = tl.tracks[trackIdx].notifies;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [this](const Notify& n) { return n.id == payload.id; }), v.end());
    }
    const char* Name() const override { return "Add Notify"; }
};

struct AddStateCmd : INotifyCommand
{
    int         trackIdx;
    NotifyState payload;

    void Apply (TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        tl.tracks[trackIdx].states.push_back(payload);
    }
    void Revert(TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        auto& v = tl.tracks[trackIdx].states;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [this](const NotifyState& s) { return s.id == payload.id; }), v.end());
    }
    const char* Name() const override { return "Add NotifyState"; }
};

struct DeleteNotifyCmd : INotifyCommand
{
    int    trackIdx;
    Notify payload;

    void Apply (TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        auto& v = tl.tracks[trackIdx].notifies;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [this](const Notify& n) { return n.id == payload.id; }), v.end());
    }
    void Revert(TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        tl.tracks[trackIdx].notifies.push_back(payload);
        std::sort(tl.tracks[trackIdx].notifies.begin(),
                  tl.tracks[trackIdx].notifies.end(),
                  [](const Notify& a, const Notify& b) { return a.time < b.time; });
    }
    const char* Name() const override { return "Delete Notify"; }
};

struct DeleteStateCmd : INotifyCommand
{
    int         trackIdx;
    NotifyState payload;

    void Apply (TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        auto& v = tl.tracks[trackIdx].states;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [this](const NotifyState& s) { return s.id == payload.id; }), v.end());
    }
    void Revert(TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        tl.tracks[trackIdx].states.push_back(payload);
    }
    const char* Name() const override { return "Delete NotifyState"; }
};

// ---- Track-level commands ----
// Reorder is MoveTrackCmd(from, to). Delete keeps the whole NotifyTrack so undo restores
// its notifies and states intact.

struct MoveTrackCmd : INotifyCommand
{
    int fromIdx;
    int toIdx;

    static void DoMove(TimelineComponent& tl, int from, int to) {
        if (from == to) return;
        if (from < 0 || from >= (int)tl.tracks.size()) return;
        if (to   < 0 || to   >= (int)tl.tracks.size()) return;
        NotifyTrack tmp = std::move(tl.tracks[from]);
        tl.tracks.erase(tl.tracks.begin() + from);
        tl.tracks.insert(tl.tracks.begin() + to, std::move(tmp));
    }
    void Apply (TimelineComponent& tl) override { DoMove(tl, fromIdx, toIdx); }
    void Revert(TimelineComponent& tl) override { DoMove(tl, toIdx, fromIdx); }
    const char* Name() const override { return "Move Track"; }
};

struct DeleteTrackCmd : INotifyCommand
{
    int         trackIdx;
    NotifyTrack payload;     // full snapshot - restored verbatim on undo

    void Apply (TimelineComponent& tl) override {
        if (trackIdx < 0 || trackIdx >= (int)tl.tracks.size()) return;
        tl.tracks.erase(tl.tracks.begin() + trackIdx);
    }
    void Revert(TimelineComponent& tl) override {
        const int clamped = std::clamp(trackIdx, 0, (int)tl.tracks.size());
        tl.tracks.insert(tl.tracks.begin() + clamped, payload);
    }
    const char* Name() const override { return "Delete Track"; }
};

// ==== NotifyTrackEditor ====

void NotifyTrackEditor::SetTarget(TimelineComponent* tl)
{
    if (m_target != tl) {
        m_target = tl;
        m_selection = {};
        m_dragMode  = DragMode::None;
        m_undoStack.clear();
        m_redoStack.clear();
        // Snap the preview clock to the entity's animation time.
        if (m_world && m_editingEntity) {
            if (auto* anim = m_world->GetComponent<AnimationComponent>(m_editingEntity))
                m_previewTime = anim->primaryTime;
        }
    }
}

void NotifyTrackEditor::RenderInWindow(const char* title, bool* pOpen)
{
    if (!ImGui::Begin(title, pOpen)) { ImGui::End(); return; }
    Render();
    ImGui::End();
}

void NotifyTrackEditor::Render()
{
    if (!m_target) {
        ImGui::TextDisabled("Select an entity with a TimelineComponent (or an "
                            "AnimationComponent — one will be created).");
        return;
    }

    // ---- Window-scoped hotkeys (Ctrl+Z / Ctrl+Y / Delete) ----
    // Do not drop the focus guard (as in SequencerWindow): a merely VISIBLE notify editor
    // would take Ctrl+Z alongside the scene layer - one keypress, two undo stacks popping.
    ImGuiIO& io = ImGui::GetIO();
    m_wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_wasFocused)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift) Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))                 Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) &&  io.KeyShift) Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))                          DeleteSelection();
    }

    DrawToolbar();
    ImGui::Separator();

    // ---- Preview tick ----
    // When playing, advance the editor scrubber and push the time into the
    // entity's AnimationComponent so the real TimelineSystem path fires.
    if (m_previewPlaying && m_target->clipDuration > 0.f) {
        m_previewTime += io.DeltaTime * m_previewSpeed;
        if (m_previewTime > m_target->clipDuration) m_previewTime = 0.f;
    }
    ApplyPreviewTimeToEntity();

    // ---- Canvas child ----
    const float canvasHeight = m_target->tracks.empty()
        ? 80.f
        : kRulerHeight + m_target->tracks.size() * kTrackHeight + 4.f;
    ImGui::BeginChild("##notifyCanvas", ImVec2(0, canvasHeight),
                      false, ImGuiWindowFlags_NoScrollbar);

    const ImVec2 canvasPos  = ImGui::GetCursorScreenPos();
    const float canvasWidth = ImGui::GetContentRegionAvail().x;
    const float tracksX     = canvasPos.x + kHeaderColumnWidth;
    const float tracksWidth = std::max(50.f, canvasWidth - kHeaderColumnWidth);

    // Keep the scroll inside the content range up-front so a stale/zoomed
    // m_scrollX never strands the tracks off-screen on the first frame.
    ClampScroll(tracksWidth);

    // Capture the mouse over the lane + ruler region with an InvisibleButton: with no item
    // under the cursor, dragging on empty canvas falls through to ImGui's "drag void = move
    // window" and the floating Timeline window slides. The header column stays uncovered so
    // its widgets (name / category / buttons) still work.
    const float laneH = std::max(1.f, kRulerHeight + m_target->tracks.size() * kTrackHeight);
    ImGui::SetCursorScreenPos(ImVec2(tracksX, canvasPos.y));
    ImGui::InvisibleButton("##tlLanes", ImVec2(tracksWidth, laneH),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool laneHovered = ImGui::IsItemHovered();
    ImGui::SetCursorScreenPos(canvasPos); // restore for drawlist + header widgets

    DrawRuler(tracksX, canvasPos.y, tracksWidth);

    for (size_t i = 0; i < m_target->tracks.size(); ++i)
    {
        const float rowY = canvasPos.y + kRulerHeight + i * kTrackHeight;
        DrawTrackHeader(static_cast<int>(i), canvasPos.x, rowY, kHeaderColumnWidth);
        DrawTrackContent(static_cast<int>(i), tracksX, rowY, tracksWidth);
    }

    const float tracksBottom = canvasPos.y + kRulerHeight
                             + m_target->tracks.size() * kTrackHeight;
    DrawCurrentTimeCursor(tracksX, canvasPos.y, tracksBottom);

    // ---- Box-select on empty timeline area ----
    // Starts on a click in the tracks region that no notify/state hit-test claimed
    // (m_dragMode == None); the corners are kept in screen coords, and on release every
    // notify/state whose visual bounds overlap the rectangle is collected.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && laneHovered
        && m_dragMode == DragMode::None
        && io.MousePos.x >= tracksX
        && io.MousePos.y >= canvasPos.y + kRulerHeight)
    {
        m_dragMode  = DragMode::BoxSelect;
        m_boxStartX = m_boxEndX = io.MousePos.x;
        m_boxStartY = m_boxEndY = io.MousePos.y;
        if (!io.KeyShift) {
            m_selection = {};
            m_multiSel.clear();
        }
    }
    if (m_dragMode == DragMode::BoxSelect) {
        m_boxEndX = io.MousePos.x;
        m_boxEndY = io.MousePos.y;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = std::min(m_boxStartX, m_boxEndX);
        const float x1 = std::max(m_boxStartX, m_boxEndX);
        const float y0 = std::min(m_boxStartY, m_boxEndY);
        const float y1 = std::max(m_boxStartY, m_boxEndY);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(120, 180, 255, 32));
        dl->AddRect      (ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(120, 180, 255, 200), 0, 0, 1.f);

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // Convert rect -> time range x track range.
            const float tLo = PixelToTime(std::max(0.f, x0 - tracksX));
            const float tHi = PixelToTime(std::max(0.f, x1 - tracksX));
            const int   rLo = std::max(0,
                static_cast<int>((y0 - (canvasPos.y + kRulerHeight)) / kTrackHeight));
            const int   rHi = std::min((int)m_target->tracks.size() - 1,
                static_cast<int>((y1 - (canvasPos.y + kRulerHeight)) / kTrackHeight));

            for (int r = rLo; r <= rHi; ++r) {
                if (r < 0 || r >= (int)m_target->tracks.size()) continue;
                auto& tr = m_target->tracks[r];
                for (const auto& n : tr.notifies) {
                    if (n.time >= tLo && n.time <= tHi)
                        AddToMultiSel({ SelKind::Notify, n.id });
                }
                for (const auto& st : tr.states) {
                    // An interval is a hit on ANY overlap.
                    if (st.endTime >= tLo && st.startTime <= tHi)
                        AddToMultiSel({ SelKind::NotifyState, st.id });
                }
            }
            // Promote the first multi-sel pick to primary so the property
            // panel has something to draw.
            if (m_selection.id == 0u && !m_multiSel.empty()) {
                m_selection = m_multiSel.front();
                m_multiSel.erase(m_multiSel.begin());
            }
            m_dragMode = DragMode::None;
        }
    }

    // ---- Auto-scroll while dragging near the canvas edge ----
    if (m_dragMode == DragMode::Move
        || m_dragMode == DragMode::ResizeStart
        || m_dragMode == DragMode::ResizeEnd
        || m_dragMode == DragMode::BoxSelect)
    {
        TickAutoScrollOnDrag(tracksX, tracksWidth);
    }

    // ---- Ctrl+C / Ctrl+V over the timeline canvas ----
    if (ImGui::IsWindowHovered() && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C)) CopySelectionToClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_V)) PasteClipboardAtTime(m_previewTime);
    }

    // Mouse-wheel zoom pivoting around the cursor. The pivot is clamped to the lane area (a
    // wheel over the header does not fling the scroll) and the scroll re-clamped so content
    // cannot be zoomed off-screen.
    if (laneHovered && io.MouseWheel != 0.f) {
        const float oldPps = m_pixelsPerSecond;
        m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * (1.f + io.MouseWheel * 0.1f),
                                       20.f, 2000.f);
        const float pivotX = std::clamp(io.MousePos.x, tracksX, tracksX + tracksWidth);
        const float mouseTimeBefore = (pivotX - tracksX + m_scrollX) / oldPps;
        m_scrollX = mouseTimeBefore * m_pixelsPerSecond - (pivotX - tracksX);
        ClampScroll(tracksWidth);
    }

    // Middle-mouse drag to pan.
    if (laneHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) {
        m_scrollX -= io.MouseDelta.x;
        ClampScroll(tracksWidth);
    }

    // Right-click opens the context menu. Gate on the lane InvisibleButton's own hover
    // (laneHovered), NOT IsWindowHovered: the button captures the right button and becomes
    // the active item on press, so IsWindowHovered reports "blocked by active item" and
    // would swallow the Add-Notify menu.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && laneHovered)
    {
        const ImVec2 mp = io.MousePos;
        const float t = PixelToTime(mp.x - tracksX);
        const int trackIdx = (mp.y >= canvasPos.y + kRulerHeight)
            ? static_cast<int>((mp.y - canvasPos.y - kRulerHeight) / kTrackHeight)
            : -1;
        if (trackIdx >= 0 && trackIdx < static_cast<int>(m_target->tracks.size())
            && t >= 0.f && t <= m_target->clipDuration)
        {
            m_pendingAddTrack = trackIdx;
            m_pendingAddTime  = std::clamp(t, 0.f, m_target->clipDuration);
            m_openContextMenu = true;
        }
    }

    ImGui::EndChild();

    HandleContextMenu();

    ImGui::Separator();
    DrawPropertyPanel();
}

// ==== Sub-draws ====

void NotifyTrackEditor::DrawToolbar()
{
    ImGui::Text("Duration:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    ImGui::DragFloat("##dur", &m_target->clipDuration, 0.05f, 0.05f, 600.f, "%.2fs");

    ImGui::SameLine();
    if (ImGui::Button(m_previewPlaying ? "Pause" : "Play")) m_previewPlaying = !m_previewPlaying;
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { m_previewPlaying = false; m_previewTime = 0.f; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::DragFloat("Speed", &m_previewSpeed, 0.02f, 0.05f, 4.f, "%.2fx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    EditorUI::DragFloat("Time##previewScrub", &m_previewTime, 0.f, m_target->clipDuration, "%.2fs");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    ImGui::DragFloat("Zoom", &m_pixelsPerSecond, 1.f, 20.f, 2000.f, "%.0f px/s");

    ImGui::SameLine();
    if (ImGui::Button("Sync Clip")) SyncDurationFromActiveClip();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pull duration from the entity's active AnimationClip");

    ImGui::SameLine();
    if (ImGui::Button("Add Track")) {
        NotifyTrack t;
        t.name = "Track " + std::to_string(m_target->tracks.size() + 1);
        t.category = NotifyCategory::Custom;
        m_target->tracks.push_back(std::move(t));
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu tracks, %zu/%zu undo)",
        m_target->tracks.size(), m_undoStack.size(), kMaxHistory);
}

void NotifyTrackEditor::DrawRuler(float canvasX, float canvasY, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(canvasX, canvasY),
                      ImVec2(canvasX + width, canvasY + kRulerHeight),
                      IM_COL32(30, 30, 30, 255));

    // Auto-tick spacing: a "nice" step giving ~80 px between ticks.
    const float targetPx = 80.f;
    const float secsPerTarget = targetPx / m_pixelsPerSecond;
    float step = 1.f;
    if      (secsPerTarget < 0.05f) step = 0.025f;
    else if (secsPerTarget < 0.10f) step = 0.05f;
    else if (secsPerTarget < 0.25f) step = 0.1f;
    else if (secsPerTarget < 0.50f) step = 0.25f;
    else if (secsPerTarget < 1.00f) step = 0.5f;
    else if (secsPerTarget < 2.50f) step = 1.f;
    else if (secsPerTarget < 5.00f) step = 2.5f;
    else                            step = 5.f;

    const float firstT = std::floor((m_scrollX / m_pixelsPerSecond) / step) * step;
    for (float t = firstT; ; t += step) {
        const float x = canvasX + TimeToPixel(t);
        if (x > canvasX + width) break;
        if (x < canvasX) continue;
        dl->AddLine(ImVec2(x, canvasY + kRulerHeight - 6),
                    ImVec2(x, canvasY + kRulerHeight),
                    IM_COL32(200, 200, 200, 255));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", t);
        dl->AddText(ImVec2(x + 2.f, canvasY + 2.f),
                    IM_COL32(200, 200, 200, 255), buf);
    }

    // Clip-end marker.
    const float endX = canvasX + TimeToPixel(m_target->clipDuration);
    if (endX >= canvasX && endX <= canvasX + width) {
        dl->AddLine(ImVec2(endX, canvasY),
                    ImVec2(endX, canvasY + kRulerHeight),
                    IM_COL32(255, 100, 100, 255), 2.f);
    }
}

void NotifyTrackEditor::DrawTrackHeader(int trackIdx, float canvasX, float rowY, float headerWidth)
{
    auto& track = m_target->tracks[trackIdx];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Anchor to the canvas-left X passed in (NOT the drifting ImGui cursor) so
    // every header row aligns to the same column.
    const float x0 = canvasX;

    const ImU32 bg = (trackIdx & 1) ? IM_COL32(38, 38, 38, 255) : IM_COL32(46, 46, 46, 255);
    dl->AddRectFilled(ImVec2(x0, rowY), ImVec2(x0 + headerWidth, rowY + kTrackHeight), bg);
    dl->AddLine(ImVec2(x0, rowY + kTrackHeight), ImVec2(x0 + headerWidth, rowY + kTrackHeight),
                IM_COL32(64, 64, 64, 255));

    // Category colour swatch on the far-left edge, for visual grouping.
    const int catClamped = std::clamp(static_cast<int>(track.category), 0,
                                      static_cast<int>(NotifyCategory::COUNT) - 1);
    dl->AddRectFilled(ImVec2(x0, rowY + 2.f), ImVec2(x0 + 4.f, rowY + kTrackHeight - 2.f),
                      kCategoryColors[catClamped]);

    ImGui::PushID(trackIdx);

    // ---- Row 1: track name (full width) ----
    char nameBuf[64];
    {
        const size_t n = std::min(track.name.size(), sizeof(nameBuf) - 1);
        std::memcpy(nameBuf, track.name.data(), n);
        nameBuf[n] = '\0';
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + 9.f, rowY + 4.f));
    ImGui::SetNextItemWidth(headerWidth - 16.f);
    if (ImGui::InputText("##nm", nameBuf, sizeof(nameBuf)))
        track.name.assign(nameBuf);

    // ---- Row 2: category | mute | reorder | delete ----
    ImGui::SetCursorScreenPos(ImVec2(x0 + 9.f, rowY + 26.f));
    int catIdx = static_cast<int>(track.category);
    ImGui::SetNextItemWidth(96.f);
    if (ImGui::Combo("##cat", &catIdx, kCategoryNames, IM_ARRAYSIZE(kCategoryNames)))
        track.category = static_cast<NotifyCategory>(catIdx);

    ImGui::SameLine(0.f, 8.f);
    ImGui::Checkbox("##mute", &track.muted);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute track");

    ImGui::SameLine(0.f, 8.f);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up) && trackIdx > 0) {
        auto cmd = std::make_unique<MoveTrackCmd>();
        cmd->fromIdx = trackIdx;
        cmd->toIdx   = trackIdx - 1;
        PushCommand(std::move(cmd));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move up");

    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && trackIdx + 1 < (int)m_target->tracks.size()) {
        auto cmd = std::make_unique<MoveTrackCmd>();
        cmd->fromIdx = trackIdx;
        cmd->toIdx   = trackIdx + 1;
        PushCommand(std::move(cmd));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move down");

    ImGui::SameLine(0.f, 8.f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 42, 42, 255));
    const bool deleteClicked = ImGui::SmallButton("X");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete track");
    if (deleteClicked) {
        auto cmd = std::make_unique<DeleteTrackCmd>();
        cmd->trackIdx = trackIdx;
        cmd->payload  = track;     // copy before destruction (Apply erases it)
        PushCommand(std::move(cmd));
        // Drop any selection that pointed into this track; re-collected on the
        // next interactive click. `track` is dangling past this point.
        m_selection = {};
        m_multiSel.clear();
    }

    ImGui::PopID();
}

void NotifyTrackEditor::DrawTrackContent(int trackIdx, float canvasX, float rowY, float width)
{
    auto& track = m_target->tracks[trackIdx];
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 bg = track.muted ? IM_COL32(28, 28, 28, 255)
                                 : ((trackIdx & 1) ? IM_COL32(32, 32, 36, 255)
                                                   : IM_COL32(40, 40, 44, 255));
    dl->AddRectFilled(ImVec2(canvasX, rowY), ImVec2(canvasX + width, rowY + kTrackHeight), bg);

    // Half-second guides.
    const float step = 0.5f;
    for (float t = std::ceil((m_scrollX / m_pixelsPerSecond) / step) * step; ; t += step) {
        const float x = canvasX + TimeToPixel(t);
        if (x > canvasX + width) break;
        dl->AddLine(ImVec2(x, rowY), ImVec2(x, rowY + kTrackHeight),
                    IM_COL32(255, 255, 255, 12));
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool lmbClick    = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool lmbReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    // ---- 1. NotifyStates (drawn first so triangles sit on top) ----
    for (auto& s : track.states)
    {
        const float x0  = canvasX + TimeToPixel(s.startTime);
        const float x1  = canvasX + TimeToPixel(s.endTime);
        if (x1 < canvasX || x0 > canvasX + width) continue;

        const float y0  = rowY + 4.f;
        const float y1  = rowY + kTrackHeight - 4.f;
        const bool sel  = (m_selection.kind == SelKind::NotifyState && m_selection.id == s.id);
        const ImU32 col = s.color ? s.color : kCategoryColors[static_cast<int>(s.category)];

        // Semi-transparent fill + opaque border.
        const ImU32 fill   = (col & 0x00FFFFFFu) | 0x60000000u;
        const ImU32 border = sel ? IM_COL32(255, 220, 100, 255) : col;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill);
        dl->AddRect      (ImVec2(x0, y0), ImVec2(x1, y1), border, 0, 0, sel ? 2.f : 1.f);

        // Resize handles: 4 px strips at each end.
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + 4, y1), IM_COL32(255, 255, 255, 180));
        dl->AddRectFilled(ImVec2(x1 - 4, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 180));

        if (!s.displayName.empty())
            dl->AddText(ImVec2(x0 + 6.f, y0 + 2.f), IM_COL32_WHITE, s.displayName.c_str());

        // Hit-test (3 regions): the rect bounds for the body, 6 px strips for the handles -
        // easier to grab than the visible 4 px.
        const bool overBody  = mouse.x >= x0 + 5.f && mouse.x <= x1 - 5.f
                            && mouse.y >= y0 && mouse.y <= y1;
        const bool overStart = mouse.x >= x0 - 3.f && mouse.x <= x0 + 5.f
                            && mouse.y >= y0 && mouse.y <= y1;
        const bool overEnd   = mouse.x >= x1 - 5.f && mouse.x <= x1 + 3.f
                            && mouse.y >= y0 && mouse.y <= y1;

        if (lmbClick && (overBody || overStart || overEnd))
        {
            Selection picked{ SelKind::NotifyState, s.id };
            if (io.KeyShift) {
                // Shift = additive selection; clicking the primary makes it
                // secondary so the new pick can drive the property panel.
                if (m_selection.id != 0u && !(m_selection == picked)) {
                    AddToMultiSel(m_selection);
                }
                m_selection = picked;
            } else if (IsInMultiSel(picked) || m_selection == picked) {
                // Clicking an already-selected item keeps the group, so the whole batch can be dragged
                // (resize handles still target only the clicked item).
                m_selection = picked;
            } else {
                SelectExclusive(picked);
            }
            m_dragStartT0     = s.startTime;
            m_dragStartT1     = s.endTime;
            m_dragStartMouseX = mouse.x;
            m_dragMode = overStart ? DragMode::ResizeStart
                       : overEnd   ? DragMode::ResizeEnd
                                   : DragMode::Move;
        }
    }

    // ---- 2. Point Notifies ----
    for (auto& n : track.notifies)
    {
        const float cx = canvasX + TimeToPixel(n.time);
        if (cx < canvasX - 12.f || cx > canvasX + width + 12.f) continue;

        const float cy   = rowY + kTrackHeight * 0.5f;
        const ImU32 col  = n.color ? n.color : kCategoryColors[static_cast<int>(n.category)];
        const bool sel   = (m_selection.kind == SelKind::Notify && m_selection.id == n.id);

        if (sel) {
            const ImU32 halo = IM_COL32(255, 220, 100, 255);
            dl->AddTriangleFilled(ImVec2(cx, cy + 11),
                                  ImVec2(cx - 10, cy - 9),
                                  ImVec2(cx + 10, cy - 9), halo);
        }
        dl->AddTriangleFilled(ImVec2(cx, cy + 8),
                              ImVec2(cx - 7, cy - 6),
                              ImVec2(cx + 7, cy - 6), col);
        dl->AddTriangle      (ImVec2(cx, cy + 8),
                              ImVec2(cx - 7, cy - 6),
                              ImVec2(cx + 7, cy - 6), IM_COL32(10,10,10,255), 1.f);

        if (!n.displayName.empty())
            dl->AddText(ImVec2(cx + 10.f, cy - 8.f), IM_COL32_WHITE, n.displayName.c_str());

        const bool hovered = mouse.x >= cx - 8.f && mouse.x <= cx + 8.f
                          && mouse.y >= cy - 10.f && mouse.y <= cy + 10.f;
        if (hovered && lmbClick) {
            Selection picked{ SelKind::Notify, n.id };
            if (io.KeyShift) {
                if (m_selection.id != 0u && !(m_selection == picked)) AddToMultiSel(m_selection);
                m_selection = picked;
            } else if (IsInMultiSel(picked) || m_selection == picked) {
                m_selection = picked;
            } else {
                SelectExclusive(picked);
            }
            m_dragStartT0     = n.time;
            m_dragStartT1     = n.time;
            m_dragStartMouseX = mouse.x;
            m_dragMode = DragMode::Move;
        }
    }

    // ---- 3. Apply the drag every frame; push a single command on release ----
    // A move drags every selected item (primary + multi); a resize only the primary (its
    // semantics are per item, not batchable).
    if ((m_dragMode == DragMode::Move) && m_selection.id != 0u)
    {
        const float dx = io.MousePos.x - m_dragStartMouseX;
        const float dt = dx / m_pixelsPerSecond;

        auto moveOne = [&](const Selection& sel, float origin) {
            if (sel.kind == SelKind::Notify) {
                if (auto* n = FindNotifyById(sel.id)) {
                    n->time = SnapTime(origin + dt, m_target->clipDuration, io.KeyShift);
                }
            } else if (sel.kind == SelKind::NotifyState) {
                if (auto* s = FindNotifyStateById(sel.id)) {
                    const float w = s->endTime - s->startTime;
                    const float maxStart = std::max(0.f, m_target->clipDuration - w);
                    float newStart = SnapTime(origin + dt, maxStart, io.KeyShift);
                    s->startTime = newStart;
                    s->endTime   = newStart + w;
                }
            }
        };

        // The primary moves from its captured origin by dt; each multi pick moves from its live
        // value by this frame's mouse delta - an approximation that saves per-item origin
        // snapshots.
        moveOne(m_selection, m_dragStartT0);
        for (const auto& sel : m_multiSel) {
            if (sel.kind == SelKind::Notify) {
                if (auto* n = FindNotifyById(sel.id))
                    n->time = SnapTime(n->time + io.MouseDelta.x / m_pixelsPerSecond,
                                       m_target->clipDuration, io.KeyShift);
            } else if (sel.kind == SelKind::NotifyState) {
                if (auto* s = FindNotifyStateById(sel.id)) {
                    const float w = s->endTime - s->startTime;
                    s->startTime = SnapTime(s->startTime + io.MouseDelta.x / m_pixelsPerSecond,
                                            m_target->clipDuration - w, io.KeyShift);
                    s->endTime   = s->startTime + w;
                }
            }
        }
    }
    else if (m_dragMode == DragMode::ResizeStart && m_selection.id != 0u)
    {
        const float dx = io.MousePos.x - m_dragStartMouseX;
        const float dt = dx / m_pixelsPerSecond;
        if (auto* s = FindNotifyStateById(m_selection.id)) {
            float ns = SnapTime(m_dragStartT0 + dt, m_target->clipDuration, io.KeyShift);
            s->startTime = std::min(ns, s->endTime - 0.01f);
        }
    }
    else if (m_dragMode == DragMode::ResizeEnd && m_selection.id != 0u)
    {
        const float dx = io.MousePos.x - m_dragStartMouseX;
        const float dt = dx / m_pixelsPerSecond;
        if (auto* s = FindNotifyStateById(m_selection.id)) {
            float ne = SnapTime(m_dragStartT1 + dt, m_target->clipDuration, io.KeyShift);
            s->endTime = std::max(ne, s->startTime + 0.01f);
        }
    }

    if (lmbReleased && m_dragMode != DragMode::None && m_selection.id != 0u)
    {
        // Push a single move / resize command capturing the start -> end delta.
        if (m_selection.kind == SelKind::Notify) {
            if (auto* n = FindNotifyById(m_selection.id);
                n && n->time != m_dragStartT0) {
                auto cmd = std::make_unique<MoveNotifyCmd>();
                cmd->id   = n->id;
                cmd->oldT = m_dragStartT0;
                cmd->newT = n->time;
                PushCommand(std::move(cmd), /*runApply=*/false);  // already applied live
            }
        }
        else if (m_selection.kind == SelKind::NotifyState) {
            if (auto* s = FindNotifyStateById(m_selection.id);
                s && (s->startTime != m_dragStartT0 || s->endTime != m_dragStartT1)) {
                auto cmd = std::make_unique<MoveStateCmd>();
                cmd->id       = s->id;
                cmd->oldStart = m_dragStartT0;
                cmd->oldEnd   = m_dragStartT1;
                cmd->newStart = s->startTime;
                cmd->newEnd   = s->endTime;
                PushCommand(std::move(cmd), /*runApply=*/false);
            }
        }
        m_dragMode = DragMode::None;
    }
}

void NotifyTrackEditor::DrawCurrentTimeCursor(float canvasX, float topY, float bottomY)
{
    const float x = canvasX + TimeToPixel(m_previewTime);
    if (x < canvasX || x > canvasX + ImGui::GetContentRegionAvail().x) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x, topY), ImVec2(x, bottomY),
                IM_COL32(255, 80, 80, 220), 1.5f);
}

void NotifyTrackEditor::HandleContextMenu()
{
    if (m_openContextMenu) {
        ImGui::OpenPopup("##notifyCtx");
        m_openContextMenu = false;
    }
    if (ImGui::BeginPopup("##notifyCtx"))
    {
        const bool validTrack = m_pendingAddTrack >= 0
                             && m_pendingAddTrack < static_cast<int>(m_target->tracks.size());

        if (ImGui::MenuItem("Add Notify Here", nullptr, false, validTrack))
        {
            auto& track = m_target->tracks[m_pendingAddTrack];
            Notify n;
            n.id          = m_target->nextNotifyId++;
            n.category    = track.category;
            n.time        = m_pendingAddTime;
            n.displayName = "notify";
            n.color       = kCategoryColors[static_cast<int>(track.category)];

            auto cmd = std::make_unique<AddNotifyCmd>();
            cmd->trackIdx = m_pendingAddTrack;
            cmd->payload  = n;
            PushCommand(std::move(cmd));
            m_selection = { SelKind::Notify, n.id };
        }
        if (ImGui::MenuItem("Add NotifyState Here", nullptr, false, validTrack))
        {
            auto& track = m_target->tracks[m_pendingAddTrack];
            NotifyState s;
            s.id        = m_target->nextNotifyId++;
            s.category  = track.category;
            s.startTime = m_pendingAddTime;
            s.endTime   = std::min(m_pendingAddTime + 0.2f, m_target->clipDuration);
            s.displayName = "interval";
            s.color     = kCategoryColors[static_cast<int>(track.category)];

            auto cmd = std::make_unique<AddStateCmd>();
            cmd->trackIdx = m_pendingAddTrack;
            cmd->payload  = s;
            PushCommand(std::move(cmd));
            m_selection = { SelKind::NotifyState, s.id };
        }
        if (m_selection.id != 0u)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate Selected"))     DuplicateSelection();
            if (ImGui::MenuItem("Delete Selected", "Del")) DeleteSelection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Undo",  "Ctrl+Z", false, !m_undoStack.empty())) Undo();
        if (ImGui::MenuItem("Redo",  "Ctrl+Y", false, !m_redoStack.empty())) Redo();
        ImGui::EndPopup();
    }
}

void NotifyTrackEditor::DrawPropertyPanel()
{
    if (m_selection.id == 0u) {
        ImGui::TextDisabled("(no notify selected)");
        return;
    }

    // Pull the common fields into refs so one UI handles Notify and NotifyState; time vs
    // interval is the only difference.
    if (m_selection.kind == SelKind::Notify) {
        Notify* n = FindNotifyById(m_selection.id);
        if (!n) { m_selection = {}; return; }

        char nameBuf[128];
        const size_t sz = std::min(n->displayName.size(), sizeof(nameBuf) - 1);
        std::memcpy(nameBuf, n->displayName.data(), sz);
        nameBuf[sz] = '\0';
        if (ImGui::InputText("Display Name", nameBuf, sizeof(nameBuf)))
            n->displayName.assign(nameBuf);

        ImGui::DragFloat("Time##notifyTime", &n->time, 0.005f, 0.f, m_target->clipDuration, "%.3fs");

        int catIdx = static_cast<int>(n->category);
        if (ImGui::Combo("Category", &catIdx, kCategoryNames, IM_ARRAYSIZE(kCategoryNames)))
            n->category = static_cast<NotifyCategory>(catIdx);

        float colf[4] = {
            ((n->color      ) & 0xFF) / 255.f,
            ((n->color >>  8) & 0xFF) / 255.f,
            ((n->color >> 16) & 0xFF) / 255.f,
            ((n->color >> 24) & 0xFF) / 255.f,
        };
        if (EditorUI::ColorEdit4("Color", colf)) {
            auto u = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.f, 1.f) * 255.f); };
            n->color = u(colf[0]) | (u(colf[1]) << 8) | (u(colf[2]) << 16) | (u(colf[3]) << 24);
        }

        ImGui::Separator();
        NotifyEditorRegistry::RenderEditor(*n);
    }
    else if (m_selection.kind == SelKind::NotifyState) {
        NotifyState* s = FindNotifyStateById(m_selection.id);
        if (!s) { m_selection = {}; return; }

        char nameBuf[128];
        const size_t sz = std::min(s->displayName.size(), sizeof(nameBuf) - 1);
        std::memcpy(nameBuf, s->displayName.data(), sz);
        nameBuf[sz] = '\0';
        if (ImGui::InputText("Display Name", nameBuf, sizeof(nameBuf)))
            s->displayName.assign(nameBuf);

        // Two time fields; keep start <= end.
        ImGui::DragFloatRange2("Interval",
            &s->startTime, &s->endTime, 0.005f,
            0.f, m_target->clipDuration, "Start %.3fs", "End %.3fs");

        int catIdx = static_cast<int>(s->category);
        if (ImGui::Combo("Category", &catIdx, kCategoryNames, IM_ARRAYSIZE(kCategoryNames)))
            s->category = static_cast<NotifyCategory>(catIdx);

        float colf[4] = {
            ((s->color      ) & 0xFF) / 255.f,
            ((s->color >>  8) & 0xFF) / 255.f,
            ((s->color >> 16) & 0xFF) / 255.f,
            ((s->color >> 24) & 0xFF) / 255.f,
        };
        if (EditorUI::ColorEdit4("Color", colf)) {
            auto u = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.f, 1.f) * 255.f); };
            s->color = u(colf[0]) | (u(colf[1]) << 8) | (u(colf[2]) << 16) | (u(colf[3]) << 24);
        }

        ImGui::Separator();
        // NotifyEditorRegistry takes a Notify&: a temporary alias backed by `s->params` lets
        // the category UI author the same PropertyBag the runtime reads.
        Notify shim;
        shim.id          = s->id;
        shim.category    = s->category;
        shim.time        = s->startTime;
        shim.params      = s->params;
        shim.displayName = s->displayName;
        shim.color       = s->color;
        NotifyEditorRegistry::RenderEditor(shim);
        // Pull edits back out of the shim into the state.
        s->params = std::move(shim.params);
    }
}

// ==== Preview integration ====

void NotifyTrackEditor::ApplyPreviewTimeToEntity()
{
    if (!m_world || m_editingEntity == 0) return;
    auto* anim = m_world->GetComponent<AnimationComponent>(m_editingEntity);
    if (!anim) return;
    // Push the editor's scrubber clock into the runtime clock so the real TimelineSystem
    // path sees this frame's delta: the "editor sees == game sees" guarantee.
    anim->primaryTime = m_previewTime;
    anim->paused      = !m_previewPlaying;
}

// ==== Helpers ====

Notify* NotifyTrackEditor::FindNotifyById(uint32_t id, int* outTrackIdx)
{
    if (id == 0u || !m_target) return nullptr;
    for (size_t i = 0; i < m_target->tracks.size(); ++i) {
        for (auto& n : m_target->tracks[i].notifies) {
            if (n.id == id) {
                if (outTrackIdx) *outTrackIdx = static_cast<int>(i);
                return &n;
            }
        }
    }
    return nullptr;
}

NotifyState* NotifyTrackEditor::FindNotifyStateById(uint32_t id, int* outTrackIdx)
{
    if (id == 0u || !m_target) return nullptr;
    for (size_t i = 0; i < m_target->tracks.size(); ++i) {
        for (auto& s : m_target->tracks[i].states) {
            if (s.id == id) {
                if (outTrackIdx) *outTrackIdx = static_cast<int>(i);
                return &s;
            }
        }
    }
    return nullptr;
}

void NotifyTrackEditor::DeleteSelection()
{
    if (!m_target || m_selection.id == 0u) return;

    // Collect all victims first: mutating the timeline while iterating would invalidate the
    // FindNotifyById pointers.
    std::vector<Selection> victims = m_multiSel;
    victims.push_back(m_selection);

    for (const auto& sel : victims) {
        int trackIdx = -1;
        if (sel.kind == SelKind::Notify) {
            if (auto* n = FindNotifyById(sel.id, &trackIdx)) {
                auto cmd = std::make_unique<DeleteNotifyCmd>();
                cmd->trackIdx = trackIdx;
                cmd->payload  = *n;
                PushCommand(std::move(cmd));
            }
        } else if (sel.kind == SelKind::NotifyState) {
            if (auto* s = FindNotifyStateById(sel.id, &trackIdx)) {
                auto cmd = std::make_unique<DeleteStateCmd>();
                cmd->trackIdx = trackIdx;
                cmd->payload  = *s;
                PushCommand(std::move(cmd));
            }
        }
    }
    m_selection = {};
    m_multiSel.clear();
}

// ==== Multi-select helpers ====

bool NotifyTrackEditor::IsInMultiSel(const Selection& s) const
{
    for (const auto& e : m_multiSel) if (e == s) return true;
    return false;
}

void NotifyTrackEditor::AddToMultiSel(const Selection& s)
{
    // Never duplicate; never include the primary selection.
    if (s.id == 0u) return;
    if (m_selection == s) return;
    if (!IsInMultiSel(s)) m_multiSel.push_back(s);
}

void NotifyTrackEditor::ClearMultiSel()
{
    m_multiSel.clear();
}

void NotifyTrackEditor::SelectExclusive(const Selection& s)
{
    m_multiSel.clear();
    m_selection = s;
}

// ==== Clipboard - copy / paste at preview time ====

void NotifyTrackEditor::CopySelectionToClipboard()
{
    if (!m_target || m_selection.id == 0u) return;

    std::vector<Selection> all = m_multiSel;
    all.push_back(m_selection);

    // Anchor on the earliest start time; paste re-anchors at the cursor.
    float boxLeft = std::numeric_limits<float>::infinity();
    int   topRow  = std::numeric_limits<int>::max();
    for (const auto& sel : all) {
        int tr = -1;
        if (sel.kind == SelKind::Notify) {
            if (auto* n = FindNotifyById(sel.id, &tr)) {
                boxLeft = std::min(boxLeft, n->time);
                topRow  = std::min(topRow, tr);
            }
        } else if (sel.kind == SelKind::NotifyState) {
            if (auto* s = FindNotifyStateById(sel.id, &tr)) {
                boxLeft = std::min(boxLeft, s->startTime);
                topRow  = std::min(topRow, tr);
            }
        }
    }
    if (!std::isfinite(boxLeft)) return;

    m_clipboard.clear();
    for (const auto& sel : all) {
        int tr = -1;
        ClipboardItem item{};
        item.kind = sel.kind;
        if (sel.kind == SelKind::Notify) {
            if (auto* n = FindNotifyById(sel.id, &tr)) {
                item.n           = *n;
                item.relTime     = n->time - boxLeft;
                item.trackOffset = tr - topRow;
                m_clipboard.push_back(std::move(item));
            }
        } else if (sel.kind == SelKind::NotifyState) {
            if (auto* s = FindNotifyStateById(sel.id, &tr)) {
                item.s           = *s;
                item.relTime     = s->startTime - boxLeft;
                item.relTime2    = s->endTime   - boxLeft;
                item.trackOffset = tr - topRow;
                m_clipboard.push_back(std::move(item));
            }
        }
    }
}

void NotifyTrackEditor::PasteClipboardAtTime(float t)
{
    if (!m_target || m_clipboard.empty()) return;

    // The "anchor track" is the primary selection's track, or 0 if no
    // pick. Paste lays items down at trackOffset relative to that.
    int anchorTrack = 0;
    if (m_selection.id != 0u) {
        int tr = -1;
        if (m_selection.kind == SelKind::Notify)         (void)FindNotifyById(m_selection.id, &tr);
        else if (m_selection.kind == SelKind::NotifyState) (void)FindNotifyStateById(m_selection.id, &tr);
        if (tr >= 0) anchorTrack = tr;
    }

    m_selection = {};
    m_multiSel.clear();

    for (const auto& item : m_clipboard) {
        const int destTrack = anchorTrack + item.trackOffset;
        if (destTrack < 0 || destTrack >= (int)m_target->tracks.size()) continue;

        if (item.kind == SelKind::Notify) {
            Notify n = item.n;
            n.id   = m_target->nextNotifyId++;
            n.time = std::clamp(t + item.relTime, 0.f, m_target->clipDuration);
            auto cmd = std::make_unique<AddNotifyCmd>();
            cmd->trackIdx = destTrack;
            cmd->payload  = n;
            const uint32_t newId = n.id;
            PushCommand(std::move(cmd));
            if (m_selection.id == 0u) m_selection = { SelKind::Notify, newId };
            else                      AddToMultiSel({ SelKind::Notify, newId });
        } else if (item.kind == SelKind::NotifyState) {
            NotifyState s = item.s;
            s.id        = m_target->nextNotifyId++;
            s.startTime = std::clamp(t + item.relTime,  0.f, m_target->clipDuration);
            s.endTime   = std::clamp(t + item.relTime2, s.startTime + 0.01f, m_target->clipDuration);
            auto cmd = std::make_unique<AddStateCmd>();
            cmd->trackIdx = destTrack;
            cmd->payload  = s;
            const uint32_t newId = s.id;
            PushCommand(std::move(cmd));
            if (m_selection.id == 0u) m_selection = { SelKind::NotifyState, newId };
            else                      AddToMultiSel({ SelKind::NotifyState, newId });
        }
    }
}

// ==== Sync clip duration + auto-scroll ====

void NotifyTrackEditor::SyncDurationFromActiveClip()
{
    if (!m_world || !m_target) return;
    // A heuristic: the canonical duration is on the clip's Resource::AnimationResource,
    // which only AnimationClipSystem reaches. Widen clipDuration to cover
    // AnimationComponent::primaryTime instead, so a too-short timeline does not cut off
    // later notifies.
    // TODO: read the active clip's duration from the clip library directly.
    auto* anim = m_world->GetComponent<AnimationComponent>(m_editingEntity);
    if (!anim) return;
    if (anim->primaryTime + 0.1f > m_target->clipDuration)
        m_target->clipDuration = std::max(1.f, anim->primaryTime + 0.5f);
}

void NotifyTrackEditor::TickAutoScrollOnDrag(float canvasX, float canvasWidth)
{
    constexpr float kEdgePx     = 24.f;   // distance from edge that triggers scroll
    constexpr float kMaxScrollV = 600.f;  // pixels / second at the very edge

    const float mx       = ImGui::GetIO().MousePos.x;
    const float distLeft = mx - canvasX;
    const float distRght = (canvasX + canvasWidth) - mx;

    float speed = 0.f;
    if (distLeft < kEdgePx)   speed = -kMaxScrollV * (1.f - distLeft / kEdgePx);
    else if (distRght < kEdgePx) speed =  kMaxScrollV * (1.f - distRght / kEdgePx);
    if (speed == 0.f) return;

    m_scrollX += speed * ImGui::GetIO().DeltaTime;
    ClampScroll(canvasWidth);

    // Bias the drag anchor by the same delta so the dragged item stays under the cursor
    // instead of snapping back while the canvas catches up (the move would feel sticky at
    // the edges).
    m_dragStartMouseX -= speed * ImGui::GetIO().DeltaTime;
}

void NotifyTrackEditor::ClampScroll(float viewWidth)
{
    if (!m_target) { m_scrollX = std::max(0.f, m_scrollX); return; }
    // Content spans [0, clipDuration] in time -> [0, clipDuration x pps] in pixels. Scroll
    // until the clip end sits a little inside the right edge, never past it; never before
    // t=0 on the left.
    const float contentW  = m_target->clipDuration * m_pixelsPerSecond;
    const float maxScroll = std::max(0.f, contentW - viewWidth + 40.f);
    m_scrollX = std::clamp(m_scrollX, 0.f, maxScroll);
}

void NotifyTrackEditor::DuplicateSelection()
{
    if (!m_target || m_selection.id == 0u) return;
    int trackIdx = -1;

    if (m_selection.kind == SelKind::Notify) {
        if (auto* src = FindNotifyById(m_selection.id, &trackIdx)) {
            Notify copy = *src;
            copy.id   = m_target->nextNotifyId++;
            copy.time = std::min(copy.time + 0.05f, m_target->clipDuration);
            auto cmd = std::make_unique<AddNotifyCmd>();
            cmd->trackIdx = trackIdx;
            cmd->payload  = std::move(copy);
            const uint32_t newId = cmd->payload.id;
            PushCommand(std::move(cmd));
            m_selection = { SelKind::Notify, newId };
        }
    } else if (m_selection.kind == SelKind::NotifyState) {
        if (auto* src = FindNotifyStateById(m_selection.id, &trackIdx)) {
            NotifyState copy = *src;
            copy.id        = m_target->nextNotifyId++;
            const float w  = copy.endTime - copy.startTime;
            copy.startTime = std::min(copy.startTime + w, m_target->clipDuration - w);
            copy.endTime   = copy.startTime + w;
            auto cmd = std::make_unique<AddStateCmd>();
            cmd->trackIdx = trackIdx;
            cmd->payload  = std::move(copy);
            const uint32_t newId = cmd->payload.id;
            PushCommand(std::move(cmd));
            m_selection = { SelKind::NotifyState, newId };
        }
    }
}

// ==== Undo / Redo ====

void NotifyTrackEditor::PushCommand(std::unique_ptr<INotifyCommand> cmd, bool runApply)
{
    if (!cmd || !m_target) return;
    if (runApply) cmd->Apply(*m_target);
    m_undoStack.push_back(std::move(cmd));
    if (m_undoStack.size() > kMaxHistory) m_undoStack.erase(m_undoStack.begin());
    m_redoStack.clear();
    if (m_onEdited) m_onEdited();
}

void NotifyTrackEditor::Undo()
{
    if (m_undoStack.empty() || !m_target) return;
    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    cmd->Revert(*m_target);
    m_redoStack.push_back(std::move(cmd));
    if (m_onEdited) m_onEdited();
}

void NotifyTrackEditor::Redo()
{
    if (m_redoStack.empty() || !m_target) return;
    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    cmd->Apply(*m_target);
    m_undoStack.push_back(std::move(cmd));
    if (m_onEdited) m_onEdited();
}

} // namespace Editor
